#include "process/process.hpp"

#include "hal/cpu.hpp"
#include "mm/pfa.hpp"
#include "process/scheduler.hpp"

namespace {

Kernel::Process::Process g_process_table[Kernel::Process::MAX_PROCESSES]{};
Kernel::Process::Process* g_current_process = nullptr;
uint32_t g_next_pid = 1U;
uint64_t g_pid_cpu_ticks[Kernel::Process::MAX_PROCESSES]{};

void reset_fd_table(Kernel::Process::Process& proc) noexcept {
    for (size_t idx = 0U; idx < Kernel::Process::MAX_FDS_PER_PROCESS; ++idx) {
        proc.fd_table[idx] = Kernel::Process::FileDescriptor{};
    }
}

void init_standard_fds(Kernel::Process::Process& proc) noexcept {
    reset_fd_table(proc);
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
            slot.state = ProcessState::Ready;
            slot.exit_status = 0;
            slot.timeslice = Scheduler::DEFAULT_TIMESLICE_TICKS;
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
            slot.timeslice = Scheduler::DEFAULT_TIMESLICE_TICKS;
            slot.cpu_ticks = 0ULL;
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

    if (proc.user_stack.phys_base != 0U && proc.user_stack.num_pages > 0U) {
        free_frames(proc.user_stack.phys_base, proc.user_stack.num_pages);
    }

    if (proc.kstack_paddr != 0U) {
        free_frames(proc.kstack_paddr, KERNEL_STACK_PAGES);
    }

    proc.address_space.destroy();
    proc.pid = 0U;
    proc.ppid = 0U;
    proc.state = ProcessState::Unused;
    proc.exit_status = 0;
    proc.timeslice = 0U;
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
    proc.timeslice = 0U;
    proc.cpu_ticks = 0ULL;
    reset_fd_table(proc);
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
        const auto st = g_process_table[idx].state;
        if (st == ProcessState::Ready || st == ProcessState::Running ||
            st == ProcessState::Blocked) {
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
    HAL::get_cpu().disable_interrupts();
    auto* const proc = g_current_process;
    if (proc != nullptr) {
        g_pid_cpu_ticks[proc->pid % MAX_PROCESSES] = proc->cpu_ticks;
        proc->exit_status = exit_code;
        proc->state = ProcessState::Zombie;
        if (proc->ppid != 0U) {
            auto* const parent = find_process_by_pid(proc->ppid);
            if (parent != nullptr) {
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