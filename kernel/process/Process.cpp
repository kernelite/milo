#include "process/Process.hpp"

#include "fs/devfs/devfs.hpp"
#include "hal/cpu.hpp"
#include "mm/heap.hpp"
#include "mm/pfa.hpp"
#include "process/scheduler.hpp"
#include "sync/IrqGuard.hpp"

#include <new>

namespace {

Kernel::Process::Process* g_process_list_head = nullptr;
Kernel::Process::Process* g_process_list_tail = nullptr;
Kernel::Process::Process* g_current_process = nullptr;
uint32_t g_next_pid = 1U;

// Lightweight circular cache preserving tick telemetry after wait4() reaps & deletes a PCB
constexpr size_t REAPED_TICKS_CACHE_SIZE = 16U;
struct ReapedTickRecord {
    uint32_t pid{0U};
    uint64_t ticks{0ULL};
};
ReapedTickRecord g_reaped_ticks_cache[REAPED_TICKS_CACHE_SIZE]{};
size_t g_reaped_ticks_cursor = 0U;

void record_reaped_ticks(uint32_t pid, uint64_t ticks) noexcept {
    if (pid == 0U) {
        return;
    }
    for (size_t i = 0U; i < REAPED_TICKS_CACHE_SIZE; ++i) {
        if (g_reaped_ticks_cache[i].pid == pid) {
            g_reaped_ticks_cache[i].ticks = ticks;
            return;
        }
    }
    g_reaped_ticks_cache[g_reaped_ticks_cursor] = ReapedTickRecord{.pid = pid, .ticks = ticks};
    g_reaped_ticks_cursor = (g_reaped_ticks_cursor + 1U) % REAPED_TICKS_CACHE_SIZE;
}

void copy_bounded_str(char* dst, const char* src, size_t cap) noexcept {
    if (dst == nullptr || cap == 0U) {
        return;
    }
    size_t idx = 0U;
    if (src != nullptr) {
        for (; idx + 1U < cap && src[idx] != '\0'; ++idx) {
            dst[idx] = src[idx];
        }
    }
    dst[idx] = '\0';
}

} // namespace

namespace Kernel::Process {

Process::~Process() noexcept = default;

bool Process::setsid() noexcept {
    if (m_pgid == m_pid) {
        return false;
    }
    m_sid = m_pid;
    m_pgid = m_pid;
    return true;
}

uint32_t Process::set_umask(uint32_t new_mask) noexcept {
    const uint32_t old_mask = m_umask;
    m_umask = new_mask & 0777U;
    return old_mask;
}

void Process::mark_stopped(uint32_t sig) noexcept {
    m_state = ProcessState::Stopped;
    m_stop_signal = sig;
    m_stop_reported = false;
}

bool Process::consume_stop_report(uint32_t& out_sig) noexcept {
    if (m_state != ProcessState::Stopped || m_stop_reported) {
        return false;
    }
    out_sig = m_stop_signal;
    m_stop_reported = true;
    return true;
}

void Process::mark_continued() noexcept {
    m_stop_signal = 0U;
    m_stop_reported = false;
    m_continued = true;
}

bool Process::consume_continued() noexcept {
    if (!m_continued) {
        return false;
    }
    m_continued = false;
    return true;
}

bool Process::tick_timeslice() noexcept {
    if (m_timeslice > 0U) {
        m_timeslice--;
    }
    return m_timeslice == 0U;
}

void Process::record_cpu_tick(bool from_user) noexcept {
    if (from_user) {
        m_utime++;
    } else {
        m_stime++;
    }
}

void Process::accumulate_child_ticks(const Process& child) noexcept {
    m_cutime += child.m_utime + child.m_cutime;
    m_cstime += child.m_stime + child.m_cstime;
}

// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
void Process::attach_elf(Loader::LoadedElf&& loaded) noexcept {
    m_address_space = static_cast<MM::AddressSpace&&>(loaded.address_space);
    m_entry_point = loaded.entry_point;
    if (loaded.max_vaddr != 0U) {
        m_brk_start = loaded.max_vaddr;
        m_brk_current = loaded.max_vaddr;
    }
    if (m_mmap_next_vaddr == 0U) {
        m_mmap_next_vaddr = HAL::get_platform_info().user_mmap_base;
    }
}

void Process::set_cwd(const char* path) noexcept {
    copy_bounded_str(m_cwd, path, sizeof(m_cwd));
}

void Process::set_exe_path(const char* path) noexcept {
    copy_bounded_str(m_exe_path, path, sizeof(m_exe_path));
}

void Process::prepare_kstack() noexcept {
    if (m_kstack_top == 0U) {
        return;
    }
    HAL::get_cpu().init_user_kstack(m_kctx, m_kstack_top, m_context);
    m_kctx_initialized = true;
}

int64_t Process::execute() noexcept {
    if (!m_address_space.is_valid()) {
        return -1;
    }

    const uintptr_t orig_root = MM::AddressSpace::current_root_paddr();
    Process* const prev_proc = g_current_process;

    m_kctx_initialized = false;
    Scheduler::enqueue(this);
    Scheduler::run_until_idle();

    MM::AddressSpace::activate_root(orig_root);
    g_current_process = prev_proc;
    return m_exit_status;
}

Process* Process::create(uint32_t ppid) noexcept {
    const Sync::IrqGuard irq_guard;

    if (active_count() >= default_rlimit_nproc()) {
        return nullptr;
    }

    void* const raw_proc = MM::kzalloc(sizeof(Process), alignof(Process));
    if (raw_proc == nullptr) {
        return nullptr;
    }
    auto* const proc = new (raw_proc) Process{};

    const uintptr_t kstack_pa = Kernel::MM::alloc_frames(KERNEL_STACK_PAGES);
    if (kstack_pa == 0U) {
        proc->~Process();
        MM::kfree(proc);
        return nullptr;
    }

    if (!proc->m_fds.init(INITIAL_FD_CAPACITY)) {
        Kernel::MM::free_frames(kstack_pa, KERNEL_STACK_PAGES);
        proc->~Process();
        MM::kfree(proc);
        return nullptr;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    proc->m_kstack_paddr = kstack_pa;
    proc->m_kstack_top = MM::phys_to_virt(kstack_pa) + (KERNEL_STACK_PAGES * page_size);

    if (g_next_pid == 0U) {
        g_next_pid = 1U;
    }
    proc->m_pid = g_next_pid++;
    proc->m_ppid = ppid;
    proc->m_pgid = (ppid == 0U) ? proc->m_pid : 0U;
    proc->m_sid = (ppid == 0U) ? proc->m_pid : 0U;
    proc->m_state = ProcessState::Ready;
    proc->m_cwd[0] = '/';
    proc->m_cwd[1] = '\0';

    proc->m_rlim_nofile_cur = DEFAULT_RLIMIT_NOFILE_CUR;
    proc->m_rlim_nofile_max = DEFAULT_RLIMIT_NOFILE_MAX;
    proc->m_rlim_nproc_cur = default_rlimit_nproc();
    proc->m_rlim_nproc_max = default_rlimit_nproc();

    Kernel::FS::File* stdin_file = nullptr;
    Kernel::FS::File* stdout_file = nullptr;
    Kernel::FS::File* stderr_file = nullptr;
    if (Kernel::FS::devfs_open_console(Kernel::FS::kOpenReadOnly, &stdin_file) != Kernel::FS::kOk ||
        Kernel::FS::devfs_open_console(Kernel::FS::kOpenWriteOnly, &stdout_file) !=
            Kernel::FS::kOk ||
        Kernel::FS::devfs_open_console(Kernel::FS::kOpenWriteOnly, &stderr_file) !=
            Kernel::FS::kOk) {
        if (stdin_file != nullptr) {
            (void)Kernel::FS::vfs_close(stdin_file);
        }
        if (stdout_file != nullptr) {
            (void)Kernel::FS::vfs_close(stdout_file);
        }
        if (stderr_file != nullptr) {
            (void)Kernel::FS::vfs_close(stderr_file);
        }
        proc->m_fds.destroy();
        Kernel::MM::free_frames(kstack_pa, KERNEL_STACK_PAGES);
        proc->~Process();
        MM::kfree(proc);
        return nullptr;
    }
    (void)proc->m_fds.install(0U, stdin_file, 0U);
    (void)proc->m_fds.install(1U, stdout_file, 0U);
    (void)proc->m_fds.install(2U, stderr_file, 0U);

    proc->m_global_prev = g_process_list_tail;
    proc->m_global_next = nullptr;
    if (g_process_list_tail != nullptr) {
        g_process_list_tail->m_global_next = proc;
    } else {
        g_process_list_head = proc;
    }
    g_process_list_tail = proc;

    return proc;
}

void Process::destroy(Process& proc) noexcept {
    {
        const Sync::IrqGuard irq_guard;

        Scheduler::remove(&proc);
        if (proc.m_active_wq != nullptr) {
            proc.m_active_wq->remove(&proc);
        }

        if (proc.m_global_prev != nullptr) {
            proc.m_global_prev->m_global_next = proc.m_global_next;
        } else if (g_process_list_head == &proc) {
            g_process_list_head = proc.m_global_next;
        }

        if (proc.m_global_next != nullptr) {
            proc.m_global_next->m_global_prev = proc.m_global_prev;
        } else if (g_process_list_tail == &proc) {
            g_process_list_tail = proc.m_global_prev;
        }

        proc.m_global_next = nullptr;
        proc.m_global_prev = nullptr;

        if (g_current_process == &proc) {
            g_current_process = nullptr;
        }

        record_reaped_ticks(proc.m_pid, proc.m_utime + proc.m_stime);

        // Idempotent teardown of dynamic fd_table, user stack, address space, kstack, and PCB
        proc.m_fds.destroy();

        if (!proc.m_address_space.is_valid() && proc.m_user_stack.phys_base != 0U &&
            proc.m_user_stack.num_pages > 0U) {
            Kernel::MM::free_frames(proc.m_user_stack.phys_base, proc.m_user_stack.num_pages);
        }
        proc.m_user_stack = UserStack{};

        proc.m_address_space.destroy();

        if (proc.m_kstack_paddr != 0U) {
            Kernel::MM::free_frames(proc.m_kstack_paddr, KERNEL_STACK_PAGES);
            proc.m_kstack_paddr = 0U;
            proc.m_kstack_top = 0U;
        }
    }

    proc.~Process();
    MM::kfree(&proc);
}

Process* Process::find_by_pid(uint32_t pid) noexcept {
    if (pid == 0U) {
        return nullptr;
    }
    for (Process* proc = g_process_list_head; proc != nullptr; proc = proc->m_global_next) {
        if (proc->m_pid == pid) {
            return proc;
        }
    }
    return nullptr;
}

Process* Process::list_head() noexcept {
    return g_process_list_head;
}

size_t Process::active_count() noexcept {
    size_t count = 0U;
    for (const Process* proc = g_process_list_head; proc != nullptr; proc = proc->m_global_next) {
        if (proc->m_state != ProcessState::Unused) {
            count++;
        }
    }
    return count;
}

bool Process::has_runnable() noexcept {
    for (const Process* proc = g_process_list_head; proc != nullptr; proc = proc->m_global_next) {
        const auto cur_state = proc->m_state;
        if (cur_state == ProcessState::Ready || cur_state == ProcessState::Running ||
            cur_state == ProcessState::Blocked) {
            return true;
        }
    }
    return false;
}

Process* Process::current() noexcept {
    return g_current_process;
}

void Process::set_current(Process* proc) noexcept {
    g_current_process = proc;
}

void init_user_cpu_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t sp_el0,
                           uint64_t argc, uintptr_t argv_vaddr) noexcept {
    HAL::get_cpu().init_user_context(ctx, entry_point, sp_el0, argc, argv_vaddr);
}

[[noreturn]] void exit_current_process(int64_t exit_code) noexcept {
    auto& cpu = HAL::get_cpu();
    cpu.disable_interrupts();

    auto* const proc = Process::current();
    if (proc == nullptr) {
        cpu.return_to_kernel(exit_code);
    }

    proc->set_exit_status(exit_code);
    proc->set_state(ProcessState::Zombie);
    record_reaped_ticks(proc->pid(), proc->utime() + proc->stime());

    // Close all open descriptors & free fd_table immediately so downstream
    // pipe readers receive EOF before the parent calls wait4()
    proc->fds().destroy();

    if (proc->clear_child_tid() != 0U) {
        const uint32_t zero_tid = 0U;
        MM::copy_to_user(proc->address_space(), proc->clear_child_tid(), &zero_tid,
                         sizeof(zero_tid));
        proc->set_clear_child_tid(0U);
    }

    // Reparent any orphaned children to PID 1
    auto* const init_proc = (proc->pid() != 1U) ? Process::find_by_pid(1U) : nullptr;
    const uint32_t adopt_pid = (init_proc != nullptr) ? 1U : 0U;
    bool adopted_zombie = false;

    for (auto* child = Process::list_head(); child != nullptr; child = child->next_global()) {
        if (child != proc && child->state() != ProcessState::Unused &&
            child->ppid() == proc->pid()) {
            child->set_ppid(adopt_pid);
            if (child->state() == ProcessState::Zombie) {
                adopted_zombie = true;
            }
        }
    }

    if (adopted_zombie && init_proc != nullptr) {
        send_signal(*init_proc, SIG_CHLD);
        init_proc->child_wait_queue().wake_all();
    }

    if (proc->ppid() != 0U) {
        auto* const parent = Process::find_by_pid(proc->ppid());
        if (parent != nullptr && parent->state() != ProcessState::Unused &&
            parent->state() != ProcessState::Zombie) {
            send_signal(*parent, SIG_CHLD);
            parent->child_wait_queue().wake_all();
        }
    }

    Scheduler::remove(proc);
    Scheduler::schedule();

    cpu.return_to_kernel(exit_code);
    while (true) {
        cpu.halt();
    }
}

uint64_t last_cpu_ticks_for_pid(uint32_t pid) noexcept {
    if (pid == 0U) {
        return 0ULL;
    }
    const Process* const active = Process::find_by_pid(pid);
    if (active != nullptr) {
        return static_cast<uint64_t>(active->utime() + active->stime());
    }
    for (size_t i = 0U; i < REAPED_TICKS_CACHE_SIZE; ++i) {
        if (g_reaped_ticks_cache[i].pid == pid) {
            return g_reaped_ticks_cache[i].ticks;
        }
    }
    return 0ULL;
}

void set_foreground_pgid(uint32_t pgid) noexcept {
    Kernel::FS::console_set_foreground_pgid(pgid);
}

uint32_t get_foreground_pgid() noexcept {
    return Kernel::FS::console_get_foreground_pgid();
}

} // namespace Kernel::Process
