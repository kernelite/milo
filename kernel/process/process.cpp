#include "process/Process.hpp"

#include "fs/devfs/devfs.hpp"
#include "hal/cpu.hpp"
#include "mm/heap.hpp"
#include "mm/pfa.hpp"
#include "process/scheduler.hpp"

#include <new>

extern "C" void* memcpy(void* dest, const void* src, size_t count) noexcept;

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

} // namespace

namespace Kernel::Process {

Process::~Process() noexcept = default;

Process* allocate_process(uint32_t ppid) noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    if (active_process_count() >= default_rlimit_nproc()) {
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
        return nullptr;
    }

    void* const raw_proc = MM::kzalloc(sizeof(Process), alignof(Process));
    if (raw_proc == nullptr) {
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
        return nullptr;
    }
    auto* const proc = new (raw_proc) Process{};

    const uintptr_t kstack_pa = Kernel::MM::alloc_frames(KERNEL_STACK_PAGES);
    if (kstack_pa == 0U) {
        proc->~Process();
        MM::kfree(proc);
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
        return nullptr;
    }

    if (!proc->fds.init(INITIAL_FD_CAPACITY)) {
        Kernel::MM::free_frames(kstack_pa, KERNEL_STACK_PAGES);
        proc->~Process();
        MM::kfree(proc);
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
        return nullptr;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    proc->kstack_paddr = kstack_pa;
    proc->kstack_top = MM::phys_to_virt(kstack_pa) + (KERNEL_STACK_PAGES * page_size);

    if (g_next_pid == 0U) {
        g_next_pid = 1U;
    }
    proc->pid = g_next_pid++;
    proc->ppid = ppid;
    proc->pgid = (ppid == 0U) ? proc->pid : 0U;
    proc->sid = (ppid == 0U) ? proc->pid : 0U;
    proc->state = ProcessState::Ready;
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';

    proc->rlim_nofile_cur = DEFAULT_RLIMIT_NOFILE_CUR;
    proc->rlim_nofile_max = DEFAULT_RLIMIT_NOFILE_MAX;
    proc->rlim_nproc_cur = default_rlimit_nproc();
    proc->rlim_nproc_max = default_rlimit_nproc();

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
        proc->fds.destroy();
        Kernel::MM::free_frames(kstack_pa, KERNEL_STACK_PAGES);
        proc->~Process();
        MM::kfree(proc);
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
        return nullptr;
    }
    (void)proc->fds.install(0U, stdin_file, 0U);
    (void)proc->fds.install(1U, stdout_file, 0U);
    (void)proc->fds.install(2U, stderr_file, 0U);

    proc->global_prev = g_process_list_tail;
    proc->global_next = nullptr;
    if (g_process_list_tail != nullptr) {
        g_process_list_tail->global_next = proc;
    } else {
        g_process_list_head = proc;
    }
    g_process_list_tail = proc;

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
    return proc;
}

void release_process(Process& proc) noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    Scheduler::remove(&proc);
    if (proc.active_wq != nullptr) {
        proc.active_wq->remove(&proc);
    }

    if (proc.global_prev != nullptr) {
        proc.global_prev->global_next = proc.global_next;
    } else if (g_process_list_head == &proc) {
        g_process_list_head = proc.global_next;
    }

    if (proc.global_next != nullptr) {
        proc.global_next->global_prev = proc.global_prev;
    } else if (g_process_list_tail == &proc) {
        g_process_list_tail = proc.global_prev;
    }

    proc.global_next = nullptr;
    proc.global_prev = nullptr;

    if (g_current_process == &proc) {
        g_current_process = nullptr;
    }

    record_reaped_ticks(proc.pid, proc.utime + proc.stime);

    // Idempotent teardown of dynamic fd_table, user stack, address space, kstack, and PCB
    proc.fds.destroy();

    if (!proc.address_space.is_valid() && proc.user_stack.phys_base != 0U &&
        proc.user_stack.num_pages > 0U) {
        Kernel::MM::free_frames(proc.user_stack.phys_base, proc.user_stack.num_pages);
    }
    proc.user_stack = UserStack{};

    proc.address_space.destroy();

    if (proc.kstack_paddr != 0U) {
        Kernel::MM::free_frames(proc.kstack_paddr, KERNEL_STACK_PAGES);
        proc.kstack_paddr = 0U;
        proc.kstack_top = 0U;
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }

    proc.~Process();
    MM::kfree(&proc);
}

Process* find_process_by_pid(uint32_t pid) noexcept {
    if (pid == 0U) {
        return nullptr;
    }
    for (Process* proc = g_process_list_head; proc != nullptr; proc = proc->global_next) {
        if (proc->pid == pid) {
            return proc;
        }
    }
    return nullptr;
}

Process* process_list_head() noexcept {
    return g_process_list_head;
}

size_t active_process_count() noexcept {
    size_t count = 0U;
    for (const Process* proc = g_process_list_head; proc != nullptr; proc = proc->global_next) {
        if (proc->state != ProcessState::Unused) {
            count++;
        }
    }
    return count;
}

bool has_runnable_processes() noexcept {
    for (const Process* proc = g_process_list_head; proc != nullptr; proc = proc->global_next) {
        const auto state = proc->state;
        if (state == ProcessState::Ready || state == ProcessState::Running ||
            state == ProcessState::Blocked) {
            return true;
        }
    }
    return false;
}

Process* current_process() noexcept {
    return g_current_process;
}

void set_current_process(Process* proc) noexcept {
    g_current_process = proc;
}

void init_user_cpu_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t sp_el0,
                           uint64_t argc, uintptr_t argv_vaddr) noexcept {
    HAL::get_cpu().init_user_context(ctx, entry_point, sp_el0, argc, argv_vaddr);
}

void prepare_process_kstack(Process& proc) noexcept {
    if (proc.kstack_top == 0U) {
        return;
    }
    HAL::get_cpu().init_user_kstack(proc.kctx, proc.kstack_top, proc.context);
    proc.kctx_initialized = true;
}

int64_t execute_process(Process& proc) noexcept {
    if (!proc.address_space.is_valid()) {
        return -1;
    }

    const uintptr_t orig_root = MM::AddressSpace::current_root_paddr();
    Process* const prev_proc = g_current_process;

    proc.kctx_initialized = false;
    Scheduler::enqueue(&proc);
    Scheduler::run_until_idle();

    MM::AddressSpace::activate_root(orig_root);
    g_current_process = prev_proc;
    return proc.exit_status;
}

[[noreturn]] void exit_current_process(int64_t exit_code) noexcept {
    auto& cpu = HAL::get_cpu();
    cpu.disable_interrupts();

    auto* const proc = current_process();
    if (proc == nullptr) {
        cpu.return_to_kernel(exit_code);
    }

    proc->exit_status = exit_code;
    proc->state = ProcessState::Zombie;
    record_reaped_ticks(proc->pid, proc->utime + proc->stime);

    // Close all open descriptors & free fd_table immediately so downstream
    // pipe readers receive EOF before the parent calls wait4()
    proc->fds.destroy();

    if (proc->clear_child_tid != 0U) {
        const uint32_t zero_tid = 0U;
        MM::copy_to_user(proc->address_space, proc->clear_child_tid, &zero_tid, sizeof(zero_tid));
        proc->clear_child_tid = 0U;
    }

    // Reparent any orphaned children to PID 1
    auto* const init_proc = (proc->pid != 1U) ? find_process_by_pid(1U) : nullptr;
    const uint32_t adopt_pid = (init_proc != nullptr) ? 1U : 0U;
    bool adopted_zombie = false;

    for (auto* child = g_process_list_head; child != nullptr; child = child->global_next) {
        if (child != proc && child->state != ProcessState::Unused && child->ppid == proc->pid) {
            child->ppid = adopt_pid;
            if (child->state == ProcessState::Zombie) {
                adopted_zombie = true;
            }
        }
    }

    if (adopted_zombie && init_proc != nullptr) {
        send_signal(*init_proc, SIG_CHLD);
        init_proc->child_wait_queue.wake_all();
    }

    if (proc->ppid != 0U) {
        auto* const parent = find_process_by_pid(proc->ppid);
        if (parent != nullptr && parent->state != ProcessState::Unused &&
            parent->state != ProcessState::Zombie) {
            send_signal(*parent, SIG_CHLD);
            parent->child_wait_queue.wake_all();
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
    const Process* const active = find_process_by_pid(pid);
    if (active != nullptr) {
        return static_cast<uint64_t>(active->utime + active->stime);
    }
    for (size_t i = 0U; i < REAPED_TICKS_CACHE_SIZE; ++i) {
        if (g_reaped_ticks_cache[i].pid == pid) {
            return g_reaped_ticks_cache[i].ticks;
        }
    }
    return 0ULL;
}

// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
void attach_loaded_elf(Process& proc, Loader::LoadedElf&& loaded) noexcept {
    proc.address_space = static_cast<MM::AddressSpace&&>(loaded.address_space);
    proc.entry_point = loaded.entry_point;
    if (loaded.max_vaddr != 0U) {
        proc.brk_start = loaded.max_vaddr;
        proc.brk_current = loaded.max_vaddr;
    }
    if (proc.mmap_next_vaddr == 0U) {
        proc.mmap_next_vaddr = HAL::get_platform_info().user_mmap_base;
    }
}

void set_foreground_pgid(uint32_t pgid) noexcept {
    Kernel::FS::console_set_foreground_pgid(pgid);
}

uint32_t get_foreground_pgid() noexcept {
    return Kernel::FS::console_get_foreground_pgid();
}

} // namespace Kernel::Process