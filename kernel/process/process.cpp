#include "process/process.hpp"

#include "mm/pfa.hpp"

namespace {

extern "C" uint64_t kernel_ctx[13];

Kernel::Process::Process g_process_table[Kernel::Process::MAX_PROCESSES]{};
Kernel::Process::Process* g_current_process = nullptr;
uint32_t g_next_pid = 1U;

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
            slot.address_space = MM::AddressSpace{};
            slot.context = HAL::CpuContext{};
            slot.user_stack = UserStack{};
            slot.entry_point = 0U;
            slot.brk_start = platform.user_brk_base;
            slot.brk_current = platform.user_brk_base;
            slot.mmap_next_vaddr = platform.user_mmap_base;
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

    proc.address_space.destroy();
    proc.pid = 0U;
    proc.ppid = 0U;
    proc.state = ProcessState::Unused;
    proc.exit_status = 0;
    proc.context = HAL::CpuContext{};
    proc.user_stack = UserStack{};
    proc.entry_point = 0U;
    proc.brk_start = 0U;
    proc.brk_current = 0U;
    proc.mmap_next_vaddr = 0U;
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

int64_t execute_process(Process& proc) noexcept {
    if (!proc.address_space.is_valid()) {
        return -1;
    }

    const uintptr_t orig_root = MM::AddressSpace::current_root_paddr();
    Process* const prev_proc = g_current_process;
    uint64_t saved_kernel_ctx[13]{};
    for (size_t idx = 0U; idx < 13U; ++idx) {
        saved_kernel_ctx[idx] = kernel_ctx[idx];
    }

    proc.state = ProcessState::Running;
    g_current_process = &proc;
    proc.address_space.activate();

    const int64_t exit_code = HAL::get_cpu().enter_user_context(proc.context);

    for (size_t idx = 0U; idx < 13U; ++idx) {
        kernel_ctx[idx] = saved_kernel_ctx[idx];
    }
    MM::AddressSpace::activate_root(orig_root);
    g_current_process = prev_proc;
    return exit_code;
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