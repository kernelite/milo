#include "process/Process.hpp"

#include "fs/devfs/devfs.hpp"
#include "hal/cpu.hpp"
#include "mm/heap.hpp"
#include "mm/pfa.hpp"
#include "process/scheduler.hpp"

#include <new>

extern "C" void* memcpy(void* dest, const void* src, size_t count) noexcept;

namespace {
constexpr int ERR_EMFILE = -24;

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

// Linux AArch64 rt_sigframe layout pushed onto EL0 stack
struct alignas(16) RtSigFrame {
    uint64_t magic;
    uint64_t saved_mask;
    uint64_t regs[31];
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
    uint64_t tpidr_el0;
    // 2-instruction return trampoline: mov x8, #139 (SYS_RT_SIGRETURN); svc #0
    uint32_t trampoline[2];
    uint64_t _pad;
};
static_assert((sizeof(RtSigFrame) % 16U) == 0U, "RtSigFrame must be 16-byte aligned");

constexpr uint64_t RT_SIGFRAME_MAGIC = 0x5349474652414D45ULL;

bool is_stop_signal(uint32_t sig) noexcept {
    return sig == Kernel::Process::SIG_STOP || sig == Kernel::Process::SIG_TSTP ||
           sig == Kernel::Process::SIG_TTIN || sig == Kernel::Process::SIG_TTOU;
}

} // namespace

namespace Kernel::Process {

Process::~Process() noexcept {
    destroy_fd_table(*this);
}

bool init_fd_table(Process& proc, size_t initial_capacity) noexcept {
    if (proc.fd_table != nullptr) {
        destroy_fd_table(proc);
    }
    if (initial_capacity == 0U) {
        initial_capacity = INITIAL_FD_CAPACITY;
    }

    auto* const table = static_cast<FileDescriptor*>(
        MM::kzalloc(initial_capacity * sizeof(FileDescriptor), alignof(FileDescriptor)));
    if (table == nullptr) {
        return false;
    }

    for (size_t idx = 0U; idx < initial_capacity; ++idx) {
        new (&table[idx]) FileDescriptor{};
    }

    proc.fd_table = table;
    proc.fd_capacity = initial_capacity;
    return true;
}

void destroy_fd_table(Process& proc) noexcept {
    if (proc.fd_table == nullptr) {
        proc.fd_capacity = 0U;
        return;
    }

    for (size_t idx = 0U; idx < proc.fd_capacity; ++idx) {
        if (proc.fd_table[idx].is_open()) {
            close_fd(proc, idx);
        }
    }

    MM::kfree(proc.fd_table);
    proc.fd_table = nullptr;
    proc.fd_capacity = 0U;
}

bool ensure_fd_capacity(Process& proc, size_t required_slots) noexcept {
    if (required_slots > proc.rlim_nofile_cur) {
        return false;
    }
    if (proc.fd_table != nullptr && required_slots <= proc.fd_capacity) {
        return true;
    }

    size_t new_capacity = (proc.fd_capacity > 0U) ? proc.fd_capacity : INITIAL_FD_CAPACITY;
    while (new_capacity < required_slots) {
        if (new_capacity >= proc.rlim_nofile_cur) {
            new_capacity = static_cast<size_t>(proc.rlim_nofile_cur);
            break;
        }
        if (new_capacity > (SIZE_MAX / 2U)) {
            new_capacity = static_cast<size_t>(proc.rlim_nofile_cur);
            break;
        }
        const size_t doubled = new_capacity * 2U;
        new_capacity =
            (doubled > proc.rlim_nofile_cur) ? static_cast<size_t>(proc.rlim_nofile_cur) : doubled;
    }

    auto* const grown = static_cast<FileDescriptor*>(MM::krealloc(
        proc.fd_table, new_capacity * sizeof(FileDescriptor), alignof(FileDescriptor)));
    if (grown == nullptr) {
        return false;
    }

    for (size_t idx = proc.fd_capacity; idx < new_capacity; ++idx) {
        new (&grown[idx]) FileDescriptor{};
    }

    proc.fd_table = grown;
    proc.fd_capacity = new_capacity;
    return true;
}

int allocate_fd(Process& proc, size_t min_fd) noexcept {
    const auto max_fds = static_cast<size_t>(proc.rlim_nofile_cur);
    if (min_fd >= max_fds) {
        return ERR_EMFILE;
    }

    if (proc.fd_table == nullptr || min_fd >= proc.fd_capacity) {
        if (!ensure_fd_capacity(proc, min_fd + 1U)) {
            return ERR_EMFILE;
        }
    }

    const size_t search_limit = (proc.fd_capacity < max_fds) ? proc.fd_capacity : max_fds;
    for (size_t idx = min_fd; idx < search_limit; ++idx) {
        if (!proc.fd_table[idx].is_open()) {
            return static_cast<int>(idx);
        }
    }

    if (proc.fd_capacity >= max_fds) {
        return ERR_EMFILE;
    }

    const size_t next_slot = proc.fd_capacity;
    if (!ensure_fd_capacity(proc, next_slot + 1U)) {
        return ERR_EMFILE;
    }

    return static_cast<int>(next_slot);
}

void close_fd(Process& proc, size_t fd_idx) noexcept {
    if (proc.fd_table == nullptr || fd_idx >= proc.fd_capacity) {
        return;
    }

    auto& desc = proc.fd_table[fd_idx];
    if (!desc.is_open()) {
        desc = FileDescriptor{};
        return;
    }

    (void)Kernel::FS::vfs_close(desc.file);
    desc = FileDescriptor{};
}

void copy_fd_slot(FileDescriptor& dst, const FileDescriptor& src, bool clear_cloexec) noexcept {
    dst = src;
    if (clear_cloexec) {
        dst.fd_flags &= ~FD_FLAG_CLOEXEC;
    }
    if (dst.is_open()) {
        Kernel::FS::file_get(dst.file);
    }
}

void close_cloexec_fds(Process& proc) noexcept {
    if (proc.fd_table == nullptr) {
        return;
    }
    for (size_t idx = 0U; idx < proc.fd_capacity; ++idx) {
        if (proc.fd_table[idx].is_open() && (proc.fd_table[idx].fd_flags & FD_FLAG_CLOEXEC) != 0U) {
            close_fd(proc, idx);
        }
    }
}

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
    proc->fd_table = nullptr;
    proc->fd_capacity = 0U;

    const uintptr_t kstack_pa = Kernel::MM::alloc_frames(KERNEL_STACK_PAGES);
    if (kstack_pa == 0U) {
        proc->~Process();
        MM::kfree(proc);
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
        return nullptr;
    }

    if (!init_fd_table(*proc, INITIAL_FD_CAPACITY)) {
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
        destroy_fd_table(*proc);
        Kernel::MM::free_frames(kstack_pa, KERNEL_STACK_PAGES);
        proc->~Process();
        MM::kfree(proc);
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
        return nullptr;
    }
    proc->fd_table[0] = FileDescriptor{.file = stdin_file, .fd_flags = 0U};
    proc->fd_table[1] = FileDescriptor{.file = stdout_file, .fd_flags = 0U};
    proc->fd_table[2] = FileDescriptor{.file = stderr_file, .fd_flags = 0U};

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
    destroy_fd_table(proc);

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

void send_signal(Process& target, uint32_t sig) noexcept {
    if (sig == 0U || sig > MAX_SIGNALS || target.state == ProcessState::Unused ||
        target.state == ProcessState::Zombie) {
        return;
    }

    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    if (sig == SIG_CONT) {
        target.pending_signals &= ~((1ULL << (SIG_STOP - 1U)) | (1ULL << (SIG_TSTP - 1U)) |
                                    (1ULL << (SIG_TTIN - 1U)) | (1ULL << (SIG_TTOU - 1U)));
        if (target.state == ProcessState::Stopped) {
            target.stop_signal = 0U;
            target.stop_reported = false;
            target.continued = true;
            Scheduler::enqueue(&target);
            if (target.ppid != 0U) {
                auto* const parent = find_process_by_pid(target.ppid);
                if (parent != nullptr) {
                    parent->child_wait_queue.wake_all();
                }
            }
        }
    }

    target.pending_signals |= (1ULL << (sig - 1U));

    if (target.state == ProcessState::Blocked) {
        const uint64_t bit = 1ULL << (sig - 1U);
        const bool is_unmaskable = (sig == SIG_KILL || sig == SIG_STOP);
        const bool is_blocked = ((target.blocked_signals & bit) != 0ULL) && !is_unmaskable;
        const uint64_t handler = target.sigactions[sig - 1U].handler;
        const bool is_ignored =
            !is_unmaskable && (handler == KERNEL_SIG_IGN ||
                               (handler == KERNEL_SIG_DFL && (sig == SIG_CHLD || sig == SIG_CONT)));
        if (!is_blocked && !is_ignored) {
            if (target.active_wq != nullptr) {
                target.active_wq->remove(&target);
            }
            Scheduler::enqueue(&target);
        }
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void signal_process_group(uint32_t pgid, uint32_t sig) noexcept {
    if (pgid == 0U || sig == 0U || sig > MAX_SIGNALS) {
        return;
    }
    for (Process* proc = g_process_list_head; proc != nullptr; proc = proc->global_next) {
        if (proc->state != ProcessState::Unused && proc->state != ProcessState::Zombie &&
            proc->pgid == pgid) {
            send_signal(*proc, sig);
        }
    }
}

bool has_unblocked_pending_signal(const Process& proc) noexcept {
    const uint64_t unmaskable = (1ULL << (SIG_KILL - 1U)) | (1ULL << (SIG_STOP - 1U));
    const uint64_t deliverable = proc.pending_signals & (~proc.blocked_signals | unmaskable);
    if (deliverable == 0ULL) {
        return false;
    }

    for (uint32_t sig = 1U; sig <= MAX_SIGNALS; ++sig) {
        if ((deliverable & (1ULL << (sig - 1U))) == 0ULL) {
            continue;
        }
        if (sig == SIG_KILL || sig == SIG_STOP) {
            return true;
        }
        const uint64_t handler = proc.sigactions[sig - 1U].handler;
        if (handler == KERNEL_SIG_IGN) {
            continue;
        }
        if (handler == KERNEL_SIG_DFL && (sig == SIG_CHLD || sig == SIG_CONT)) {
            continue;
        }
        return true;
    }
    return false;
}

void deliver_pending_signals(HAL::CpuContext& ctx) noexcept {
    auto* const proc = g_current_process;
    if (proc == nullptr) {
        return;
    }

    if (proc->pending_signals == 0ULL) {
        if (proc->has_saved_sigmask) {
            proc->blocked_signals = proc->saved_sigmask;
            proc->has_saved_sigmask = false;
        }
        return;
    }

    const uint64_t unmaskable = (1ULL << (SIG_KILL - 1U)) | (1ULL << (SIG_STOP - 1U));
    const uint64_t deliverable = proc->pending_signals & (~proc->blocked_signals | unmaskable);
    if (deliverable == 0ULL) {
        if (proc->has_saved_sigmask) {
            proc->blocked_signals = proc->saved_sigmask;
            proc->has_saved_sigmask = false;
        }
        return;
    }

    uint32_t sig = 0U;
    for (uint32_t sig_num = 1U; sig_num <= MAX_SIGNALS; ++sig_num) {
        if ((deliverable & (1ULL << (sig_num - 1U))) != 0ULL) {
            sig = sig_num;
            break;
        }
    }
    if (sig == 0U) {
        return;
    }

    proc->pending_signals &= ~(1ULL << (sig - 1U));

    if (sig == SIG_KILL) {
        exit_current_process(128 + static_cast<int64_t>(sig));
    }

    const SigAction& act = proc->sigactions[sig - 1U];
    if (act.handler == KERNEL_SIG_IGN) {
        if (proc->has_saved_sigmask) {
            proc->blocked_signals = proc->saved_sigmask;
            proc->has_saved_sigmask = false;
        }
        return;
    }

    if (act.handler == KERNEL_SIG_DFL) {
        if (sig == SIG_CHLD || sig == SIG_CONT) {
            if (proc->has_saved_sigmask) {
                proc->blocked_signals = proc->saved_sigmask;
                proc->has_saved_sigmask = false;
            }
            return;
        }
        if (is_stop_signal(sig)) {
            if (proc->has_saved_sigmask) {
                proc->blocked_signals = proc->saved_sigmask;
                proc->has_saved_sigmask = false;
            }
            HAL::get_cpu().disable_interrupts();
            proc->state = ProcessState::Stopped;
            proc->stop_signal = sig;
            proc->stop_reported = false;
            if (proc->ppid != 0U) {
                auto* const parent = find_process_by_pid(proc->ppid);
                if (parent != nullptr) {
                    send_signal(*parent, SIG_CHLD);
                    parent->child_wait_queue.wake_all();
                }
            }
            Scheduler::schedule();
            return;
        }
        // Default termination action
        exit_current_process(128 + static_cast<int64_t>(sig));
    }

    // Custom EL0 signal handler: build RtSigFrame on user stack
    auto& cpu = HAL::get_cpu();
    const uintptr_t old_sp = cpu.get_user_stack_pointer(ctx);
    if (old_sp <= sizeof(RtSigFrame) + 64U) {
        exit_current_process(128 + static_cast<int64_t>(SIG_KILL));
    }

    const uintptr_t frame_vaddr = (old_sp - sizeof(RtSigFrame)) & ~0xFULL;
    RtSigFrame frame{};
    frame.magic = RT_SIGFRAME_MAGIC;
    frame.saved_mask = proc->has_saved_sigmask ? proc->saved_sigmask : proc->blocked_signals;
    proc->has_saved_sigmask = false;

    for (size_t i = 0U; i < 31U; ++i) {
        frame.regs[i] = cpu.get_reg(ctx, i);
    }
    frame.sp_el0 = old_sp;
    frame.elr_el1 = cpu.get_user_pc(ctx);
    frame.spsr_el1 = cpu.get_user_pstate(ctx);
    frame.tpidr_el0 = cpu.get_user_tls_pointer(ctx);
    frame.trampoline[0] = 0xD2801168U; // mov x8, #139 (SYS_RT_SIGRETURN)
    frame.trampoline[1] = 0xD4000001U; // svc #0

    if (MM::copy_to_user(proc->address_space, frame_vaddr, &frame, sizeof(frame)) < 0) {
        exit_current_process(128 + static_cast<int64_t>(SIG_KILL));
    }

    // Flush instruction cache if fallback stack trampoline is used
    uintptr_t tramp_paddr = 0U;
    HAL::PageFlags tramp_flags = HAL::PageFlags::None;
    const uintptr_t tramp_vaddr = frame_vaddr + offsetof(RtSigFrame, trampoline);
    if (proc->address_space.query_user_page(tramp_vaddr, tramp_paddr, tramp_flags)) {
        cpu.flush_dcache_range(MM::phys_to_virt(tramp_paddr), sizeof(frame.trampoline));
        cpu.invalidate_icache();
    }

    const uintptr_t restorer_lr = ((act.flags & SA_FLAG_RESTORER) != 0ULL && act.restorer != 0ULL)
                                      ? static_cast<uintptr_t>(act.restorer)
                                      : tramp_vaddr;

    proc->blocked_signals |= act.mask | (1ULL << (sig - 1U));
    proc->blocked_signals &= ~unmaskable;

    cpu.set_user_stack_pointer(ctx, frame_vaddr);
    cpu.set_user_pc(ctx, static_cast<uintptr_t>(act.handler));
    cpu.set_reg(ctx, 0U, sig);
    cpu.set_reg(ctx, 1U, 0ULL);
    cpu.set_reg(ctx, 2U, frame_vaddr);
    cpu.set_reg(ctx, 30U, restorer_lr);
}

int64_t restore_signal_frame(HAL::CpuContext& ctx) noexcept {
    auto* const proc = g_current_process;
    if (proc == nullptr) {
        return -1;
    }

    auto& cpu = HAL::get_cpu();
    const uintptr_t frame_vaddr = cpu.get_user_stack_pointer(ctx);
    RtSigFrame frame{};
    if (MM::copy_from_user(proc->address_space, &frame, frame_vaddr, sizeof(frame)) < 0) {
        exit_current_process(128 + static_cast<int64_t>(SIG_KILL));
    }

    if (frame.magic != RT_SIGFRAME_MAGIC) {
        exit_current_process(128 + static_cast<int64_t>(SIG_KILL));
    }

    const uint64_t unmaskable = (1ULL << (SIG_KILL - 1U)) | (1ULL << (SIG_STOP - 1U));
    proc->blocked_signals = frame.saved_mask & ~unmaskable;

    for (size_t i = 0U; i < 31U; ++i) {
        cpu.set_reg(ctx, i, frame.regs[i]);
    }
    cpu.set_user_stack_pointer(ctx, static_cast<uintptr_t>(frame.sp_el0));
    cpu.set_user_pc(ctx, static_cast<uintptr_t>(frame.elr_el1));
    cpu.set_user_pstate(ctx, frame.spsr_el1 & ~0x1FU); // Enforce EL0t mode
    cpu.set_user_tls_pointer(ctx, static_cast<uintptr_t>(frame.tpidr_el0));

    return static_cast<int64_t>(frame.regs[0]);
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
    destroy_fd_table(*proc);

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