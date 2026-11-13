#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "hal/trap.hpp"
#include "mm/address_space.hpp"
#include "process/process.hpp"

#include <cstddef>
#include <cstdint>

namespace {

// POSIX / Linux Generic Syscall Numbers
constexpr uint64_t SYS_WRITE = 64U;
constexpr uint64_t SYS_EXIT = 93U;
constexpr uint64_t SYS_EXIT_GROUP = 94U;
constexpr uint64_t SYS_GETPID = 172U;
constexpr uint64_t SYS_GETPPID = 173U;

// Negated POSIX Error Codes
constexpr int64_t ERR_EBADF = -9;
constexpr int64_t ERR_EFAULT = -14;
[[maybe_unused]] constexpr int64_t ERR_EINVAL = -22;
constexpr int64_t ERR_ENOSYS = -38;

constexpr size_t WRITE_CHUNK_SIZE = 256U;

bool validate_user_read_range(uintptr_t root_paddr, uintptr_t user_buf, size_t count) noexcept {
    const uintptr_t va_limit = Kernel::MM::user_addr_space_limit();
    if (user_buf >= va_limit) {
        return false;
    }
    if (count == 0U) {
        return true;
    }
    if (root_paddr == 0U || count > (va_limit - user_buf)) {
        return false;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uintptr_t page_mask = page_size - 1U;
    const uintptr_t first_page = user_buf & ~page_mask;
    const uintptr_t last_page = (user_buf + count - 1U) & ~page_mask;

    for (uintptr_t page_vaddr = first_page; page_vaddr <= last_page; page_vaddr += page_size) {
        uintptr_t out_paddr = 0U;
        HAL::PageFlags flags = HAL::PageFlags::None;
        if (!Kernel::MM::AddressSpace::query_user_page(root_paddr, page_vaddr, out_paddr, flags)) {
            return false;
        }
        if (!HAL::has_flag(flags, HAL::PageFlags::User) ||
            !HAL::has_flag(flags, HAL::PageFlags::Read)) {
            return false;
        }
    }

    return true;
}

int64_t sys_write(uint64_t raw_fd, uintptr_t user_buf, uint64_t raw_count) noexcept {
    if (raw_fd >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }
    const auto fd_idx = static_cast<size_t>(raw_fd);

    auto* const proc = Kernel::Process::current_process();
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        const auto& desc = proc->fd_table[fd_idx];
        if (!desc.in_use || !desc.is_writable()) {
            return ERR_EBADF;
        }
        if (desc.type != Kernel::Process::FdType::ConsoleOut &&
            desc.type != Kernel::Process::FdType::ConsoleErr) {
            return ERR_EBADF;
        }
        root_paddr = proc->address_space.root_paddr();
    } else {
        if (fd_idx != 1U && fd_idx != 2U) {
            return ERR_EBADF;
        }
        root_paddr = Kernel::MM::AddressSpace::current_root_paddr();
    }

    if (raw_count > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }
    const auto count = static_cast<size_t>(raw_count);

    if (!validate_user_read_range(root_paddr, user_buf, count)) {
        return ERR_EFAULT;
    }
    if (count == 0U) {
        return 0;
    }

    char kbuf[WRITE_CHUNK_SIZE]{};
    size_t total_written = 0U;

    while (total_written < count) {
        const size_t remaining = count - total_written;
        const size_t chunk = (remaining < WRITE_CHUNK_SIZE) ? remaining : WRITE_CHUNK_SIZE;

        const int64_t copy_res =
            Kernel::MM::copy_from_user(root_paddr, kbuf, user_buf + total_written, chunk);
        if (copy_res < 0) {
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : copy_res;
        }

        HAL::get_console().write(kbuf, chunk);
        total_written += chunk;
    }

    return static_cast<int64_t>(total_written);
}

[[noreturn]] void sys_exit(int64_t status) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc != nullptr) {
        proc->exit_status = status;
        proc->state = Kernel::Process::ProcessState::Zombie;
    }
    HAL::get_cpu().return_to_kernel(status);
}

int64_t sys_getpid() noexcept {
    const auto* const proc = Kernel::Process::current_process();
    return (proc != nullptr) ? static_cast<int64_t>(proc->pid) : 0;
}

int64_t sys_getppid() noexcept {
    const auto* const proc = Kernel::Process::current_process();
    return (proc != nullptr) ? static_cast<int64_t>(proc->ppid) : 0;
}

} // namespace

namespace Kernel {

class SyscallDispatcher : public HAL::TrapHandler {
  public:
    constexpr SyscallDispatcher() = default;

    void handle_trap(HAL::TrapFrame& frame) override {
        if (frame.type != HAL::TrapType::Syscall || frame.context == nullptr) {
            return;
        }

        auto& cpu = HAL::get_cpu();
        auto& ctx = *frame.context;
        const uint64_t syscall_num = cpu.get_syscall_number(ctx);
        int64_t ret_val = ERR_ENOSYS;

        switch (syscall_num) {
        case SYS_WRITE:
            ret_val = sys_write(cpu.get_syscall_arg(ctx, 0U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            sys_exit(static_cast<int64_t>(cpu.get_syscall_arg(ctx, 0U)));

        case SYS_GETPID:
            ret_val = sys_getpid();
            break;

        case SYS_GETPPID:
            ret_val = sys_getppid();
            break;

        default:
            ret_val = ERR_ENOSYS;
            break;
        }

        cpu.set_syscall_return(ctx, ret_val);
    }
};

constinit SyscallDispatcher global_syscall_dispatcher{};

} // namespace Kernel

namespace HAL {
TrapHandler& get_trap_dispatcher() noexcept {
    return Kernel::global_syscall_dispatcher;
}
} // namespace HAL