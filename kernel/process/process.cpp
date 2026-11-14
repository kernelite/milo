#include "process/process.hpp"

#include "hal/cpu.hpp"
#include "mm/pfa.hpp"
#include "process/scheduler.hpp"

extern "C" void* memcpy(void* dest, const void* src, size_t count) noexcept;

namespace {

Kernel::Process::Process g_process_table[Kernel::Process::MAX_PROCESSES]{};
Kernel::Process::Process* g_current_process = nullptr;
uint32_t g_next_pid = 1U;
uint64_t g_pid_cpu_ticks[Kernel::Process::MAX_PROCESSES]{};

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

void reset_sigactions(Kernel::Process::Process& proc) noexcept {
    proc.pending_signals = 0ULL;
    proc.blocked_signals = 0ULL;
    for (size_t i = 0U; i < Kernel::Process::MAX_SIGNALS; ++i) {
        proc.sigactions[i] = Kernel::Process::SigAction{};
    }
}

void init_standard_fds(Kernel::Process::Process& proc) noexcept {
    for (size_t idx = 0U; idx < Kernel::Process::MAX_FDS_PER_PROCESS; ++idx) {
        proc.fd_table[idx] = Kernel::Process::FileDescriptor{};
    }
    proc.fd_table[0] = Kernel::Process::FileDescriptor{
        .type = Kernel::Process::FdType::ConsoleTty,
        .flags = Kernel::Process::FD_FLAG_READ,
        .offset = 0U,
        .in_use = true,
    };
    proc.fd_table[1] = Kernel::Process::FileDescriptor{
        .type = Kernel::Process::FdType::ConsoleTty,
        .flags = Kernel::Process::FD_FLAG_WRITE,
        .offset = 0U,
        .in_use = true,
    };
    proc.fd_table[2] = Kernel::Process::FileDescriptor{
        .type = Kernel::Process::FdType::ConsoleTty,
        .flags = Kernel::Process::FD_FLAG_WRITE,
        .offset = 0U,
        .in_use = true,
    };
}

} // namespace

namespace Kernel::Process {

void close_fd(Process& proc, size_t fd_idx) noexcept {
    if (fd_idx >= MAX_FDS_PER_PROCESS) {
        return;
    }
    auto& desc = proc.fd_table[fd_idx];
    if (!desc.in_use) {
        return;
    }
    if (desc.type == FdType::PipeRead) {
        FS::release_pipe_reader(desc.pipe);
    } else if (desc.type == FdType::PipeWrite) {
        FS::release_pipe_writer(desc.pipe);
    }
    desc = FileDescriptor{};
}

void copy_fd_slot(FileDescriptor& dst, const FileDescriptor& src, bool clear_cloexec) noexcept {
    dst = src;
    if (clear_cloexec) {
        dst.flags &= ~FD_FLAG_CLOEXEC;
    }
    if (dst.in_use) {
        if (dst.type == FdType::PipeRead) {
            FS::retain_pipe_reader(dst.pipe);
        } else if (dst.type == FdType::PipeWrite) {
            FS::retain_pipe_writer(dst.pipe);
        }
    }
}

void close_cloexec_fds(Process& proc) noexcept {
    for (size_t idx = 0U; idx < MAX_FDS_PER_PROCESS; ++idx) {
        if (proc.fd_table[idx].in_use && (proc.fd_table[idx].flags & FD_FLAG_CLOEXEC) != 0U) {
            close_fd(proc, idx);
        }
    }
}

Process* allocate_process(uint32_t ppid) noexcept {
    const auto platform = HAL::get_platform_info();

    for (size_t idx = 0U; idx < MAX_PROCESSES; ++idx) {
        auto& slot = g_process_table[idx];
        if (slot.state == ProcessState::Unused) {
            const uintptr_t kstack_paddr = alloc_frames(KERNEL_STACK_PAGES);
            if (kstack_paddr == 0U) {
                return nullptr;
            }

            const size_t kstack_bytes = KERNEL_STACK_PAGES * platform.page_size;
            auto* const kstack_kva = reinterpret_cast<uint8_t*>(MM::phys_to_virt(kstack_paddr));
            for (size_t byte_idx = 0U; byte_idx < kstack_bytes; ++byte_idx) {
                kstack_kva[byte_idx] = 0U;
            }

            uint32_t candidate_pid = g_next_pid++;
            if (candidate_pid == 0U) {
                candidate_pid = g_next_pid++;
            }
            while (find_process_by_pid(candidate_pid) != nullptr) {
                candidate_pid = g_next_pid++;
                if (candidate_pid == 0U) {
                    candidate_pid = g_next_pid++;
                }
            }

            slot.pid = candidate_pid;
            slot.ppid = ppid;
            slot.pgid = candidate_pid;
            slot.sid = candidate_pid;
            slot.state = ProcessState::Ready;
            slot.exit_status = 0;
            slot.timeslice = Scheduler::DEFAULT_TIMESLICE_TICKS;
            slot.cpu_ticks = 0ULL;
            slot.address_space = MM::AddressSpace{};
            slot.context = HAL::CpuContext{};
            slot.kctx = HAL::KernelContext{};
            slot.kstack_paddr = kstack_paddr;
            slot.kstack_top = reinterpret_cast<uintptr_t>(kstack_kva) + kstack_bytes;
            slot.kctx_initialized = false;
            slot.user_stack = UserStack{};
            slot.entry_point = 0U;
            slot.brk_start = platform.user_brk_base;
            slot.brk_current = platform.user_brk_base;
            slot.mmap_next_vaddr = platform.user_mmap_base;
            slot.clear_child_tid = 0U;
            slot.child_wait_queue = WaitQueue{};
            for (size_t i = 0U; i < MAX_PATH_LEN; ++i) {
                slot.cwd[i] = '\0';
            }
            slot.cwd[0] = '/';
            reset_sigactions(slot);
            g_pid_cpu_ticks[candidate_pid % MAX_PROCESSES] = 0ULL;
            init_standard_fds(slot);
            return &slot;
        }
    }
    return nullptr;
}

void release_process(Process& proc) noexcept {
    if (g_current_process == &proc) {
        g_current_process = nullptr;
    }

    for (size_t idx = 0U; idx < MAX_FDS_PER_PROCESS; ++idx) {
        close_fd(proc, idx);
    }

    if (proc.user_stack.phys_base != 0U && proc.user_stack.num_pages > 0U) {
        free_frames(proc.user_stack.phys_base, proc.user_stack.num_pages);
    }

    if (proc.kstack_paddr != 0U) {
        free_frames(proc.kstack_paddr, KERNEL_STACK_PAGES);
    }

    proc.address_space.destroy();
    proc.pid = 0U;
    proc.ppid = 0U;
    proc.pgid = 0U;
    proc.sid = 0U;
    proc.state = ProcessState::Unused;
    proc.exit_status = 0;
    proc.timeslice = 0U;
    proc.cpu_ticks = 0ULL;
    proc.context = HAL::CpuContext{};
    proc.kctx = HAL::KernelContext{};
    proc.kstack_paddr = 0U;
    proc.kstack_top = 0U;
    proc.kctx_initialized = false;
    proc.user_stack = UserStack{};
    proc.entry_point = 0U;
    proc.brk_start = 0U;
    proc.brk_current = 0U;
    proc.mmap_next_vaddr = 0U;
    proc.clear_child_tid = 0U;
    proc.child_wait_queue = WaitQueue{};
    reset_sigactions(proc);
}

Process* find_process_by_pid(uint32_t pid) noexcept {
    if (pid == 0U) {
        return nullptr;
    }
    for (size_t idx = 0U; idx < MAX_PROCESSES; ++idx) {
        if (g_process_table[idx].state != ProcessState::Unused && g_process_table[idx].pid == pid) {
            return &g_process_table[idx];
        }
    }
    return nullptr;
}

size_t active_process_count() noexcept {
    size_t count = 0U;
    for (size_t idx = 0U; idx < MAX_PROCESSES; ++idx) {
        if (g_process_table[idx].state != ProcessState::Unused) {
            count++;
        }
    }
    return count;
}

bool has_runnable_processes() noexcept {
    for (size_t idx = 0U; idx < MAX_PROCESSES; ++idx) {
        const auto state = g_process_table[idx].state;
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
    target.pending_signals |= (1ULL << (sig - 1U));
    if (target.state == ProcessState::Blocked && sig == SIG_KILL) {
        Scheduler::enqueue(&target);
    }
}

void deliver_pending_signals(HAL::CpuContext& ctx) noexcept {
    auto* const proc = g_current_process;
    if (proc == nullptr || proc->pending_signals == 0ULL) {
        return;
    }

    const uint64_t unmaskable = (1ULL << (SIG_KILL - 1U)) | (1ULL << (SIG_STOP - 1U));
    const uint64_t deliverable = proc->pending_signals & (~proc->blocked_signals | unmaskable);
    if (deliverable == 0ULL) {
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
        return;
    }

    if (act.handler == KERNEL_SIG_DFL) {
        if (sig == SIG_CHLD || sig == SIG_CONT) {
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
    frame.saved_mask = proc->blocked_signals;
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
    HAL::get_cpu().disable_interrupts();
    auto* const proc = g_current_process;
    if (proc != nullptr) {
        for (size_t idx = 0U; idx < MAX_FDS_PER_PROCESS; ++idx) {
            close_fd(*proc, idx);
        }
        g_pid_cpu_ticks[proc->pid % MAX_PROCESSES] = proc->cpu_ticks;
        proc->exit_status = exit_code;
        proc->state = ProcessState::Zombie;
        if (proc->ppid != 0U) {
            auto* const parent = find_process_by_pid(proc->ppid);
            if (parent != nullptr) {
                send_signal(*parent, SIG_CHLD);
                parent->child_wait_queue.wake_all();
            }
        }
    }

    Scheduler::schedule();

    while (true) {
        HAL::get_cpu().halt();
    }
}

uint64_t last_cpu_ticks_for_pid(uint32_t pid) noexcept {
    if (pid == 0U) {
        return 0ULL;
    }
    return g_pid_cpu_ticks[pid % MAX_PROCESSES];
}

void attach_loaded_elf(Process& proc, Loader::LoadedElf&& loaded) noexcept {
    proc.address_space = static_cast<MM::AddressSpace&&>(loaded.address_space);
    proc.entry_point = loaded.entry_point;
    if (loaded.max_vaddr != 0U) {
        proc.brk_start = loaded.max_vaddr;
        proc.brk_current = loaded.max_vaddr;
    }
}

Process* process_at(size_t index) noexcept {
    if (index >= MAX_PROCESSES) {
        return nullptr;
    }
    return &g_process_table[index];
}

} // namespace Kernel::Process