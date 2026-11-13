#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "hal/trap.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"
#include "process/process.hpp"

#include <cstddef>
#include <cstdint>

namespace {

// POSIX / Linux Generic Syscall Numbers
constexpr uint64_t SYS_IOCTL = 29U;
constexpr uint64_t SYS_READ = 63U;
constexpr uint64_t SYS_WRITE = 64U;
constexpr uint64_t SYS_EXIT = 93U;
constexpr uint64_t SYS_EXIT_GROUP = 94U;
constexpr uint64_t SYS_GETPID = 172U;
constexpr uint64_t SYS_GETPPID = 173U;
constexpr uint64_t SYS_BRK = 214U;
constexpr uint64_t SYS_MUNMAP = 215U;
constexpr uint64_t SYS_MMAP = 222U;

// Negated POSIX Error Codes
constexpr int64_t ERR_EBADF = -9;
constexpr int64_t ERR_ENOMEM = -12;
constexpr int64_t ERR_EFAULT = -14;
constexpr int64_t ERR_EINVAL = -22;
constexpr int64_t ERR_ENOTTY = -25;
constexpr int64_t ERR_ENOSYS = -38;

// Linux asm-generic mmap Protection & Mapping Flags
constexpr uint64_t PROT_NONE = 0x0U;
constexpr uint64_t PROT_READ = 0x1U;
constexpr uint64_t PROT_WRITE = 0x2U;
constexpr uint64_t PROT_EXEC = 0x4U;

constexpr uint64_t MAP_SHARED = 0x01U;
constexpr uint64_t MAP_PRIVATE = 0x02U;
constexpr uint64_t MAP_FIXED = 0x10U;
constexpr uint64_t MAP_ANONYMOUS = 0x20U;

// Downward-growing anonymous mmap arena floor (ceiling is below 64 KiB stack at 0x3FFE0000)
constexpr uintptr_t MMAP_ARENA_FLOOR = 0x0000000030000000ULL;

// Linux asm-generic Terminal ioctl Request Codes
constexpr uint64_t IOCTL_TCGETS = 0x5401U;
constexpr uint64_t IOCTL_TCSETS = 0x5402U;
constexpr uint64_t IOCTL_TCSETSW = 0x5403U;
constexpr uint64_t IOCTL_TCSETSF = 0x5404U;
constexpr uint64_t IOCTL_TIOCGWINSZ = 0x5413U;

// Linux asm-generic termios Flag Bits
constexpr uint32_t TERMIOS_ICRNL = 0x0100U;
constexpr uint32_t TERMIOS_OPOST = 0x0001U;
constexpr uint32_t TERMIOS_ONLCR = 0x0004U;
constexpr uint32_t TERMIOS_CS8 = 0x0030U;
constexpr uint32_t TERMIOS_CREAD = 0x0080U;
constexpr uint32_t TERMIOS_CLOCAL = 0x0800U;
constexpr uint32_t TERMIOS_ISIG = 0x0001U;
constexpr uint32_t TERMIOS_ICANON = 0x0002U;
constexpr uint32_t TERMIOS_ECHO = 0x0008U;
constexpr uint32_t TERMIOS_ECHOE = 0x0010U;
constexpr uint32_t TERMIOS_ECHOK = 0x0020U;

constexpr size_t TERMIOS_NCCS = 19U;
constexpr uint16_t DEFAULT_TTY_ROWS = 24U;
constexpr uint16_t DEFAULT_TTY_COLS = 80U;

constexpr size_t READ_CHUNK_SIZE = 256U;
constexpr size_t WRITE_CHUNK_SIZE = 256U;

struct ktermios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[TERMIOS_NCCS];
};
static_assert(sizeof(ktermios) == 36U, "ktermios must match Linux asm-generic ABI (36 bytes)");

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};
static_assert(sizeof(winsize) == 8U, "winsize must be 8 bytes");

ktermios g_console_termios{
    .c_iflag = TERMIOS_ICRNL,
    .c_oflag = TERMIOS_OPOST | TERMIOS_ONLCR,
    .c_cflag = TERMIOS_CS8 | TERMIOS_CREAD | TERMIOS_CLOCAL,
    .c_lflag = TERMIOS_ISIG | TERMIOS_ICANON | TERMIOS_ECHO | TERMIOS_ECHOE | TERMIOS_ECHOK,
    .c_line = 0U,
    .c_cc = {0x03U, 0x1CU, 0x7FU, 0x15U, 0x04U, 0x00U, 0x01U, 0x00U, 0x11U, 0x13U, 0x1AU, 0x00U,
             0x12U, 0x0FU, 0x17U, 0x16U, 0x00U, 0x00U, 0x00U},
};

void unmap_and_free_range(Kernel::MM::AddressSpace& adrsp, uintptr_t start_vaddr,
                          uintptr_t end_vaddr, size_t page_size) noexcept {
    const uintptr_t page_mask = page_size - 1U;
    for (uintptr_t page_vaddr = start_vaddr; page_vaddr < end_vaddr; page_vaddr += page_size) {
        uintptr_t out_paddr = 0U;
        HAL::PageFlags out_flags = HAL::PageFlags::None;
        if (adrsp.query_user_page(page_vaddr, out_paddr, out_flags)) {
            adrsp.unmap_user_page(page_vaddr);
            free_frame(out_paddr & ~page_mask);
        }
    }
}

bool validate_user_range(uintptr_t root_paddr, uintptr_t user_buf, size_t count,
                         HAL::PageFlags required_perm) noexcept {
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
        if (!HAL::has_flag(flags, HAL::PageFlags::User) || !HAL::has_flag(flags, required_perm)) {
            return false;
        }
    }

    return true;
}

bool validate_user_read_range(uintptr_t root_paddr, uintptr_t user_buf, size_t count) noexcept {
    return validate_user_range(root_paddr, user_buf, count, HAL::PageFlags::Read);
}

bool validate_user_write_range(uintptr_t root_paddr, uintptr_t user_buf, size_t count) noexcept {
    return validate_user_range(root_paddr, user_buf, count, HAL::PageFlags::Write);
}

int64_t sys_ioctl(uint64_t raw_fd, uint64_t request, uintptr_t arg) noexcept {
    if (raw_fd >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }
    const auto fd_idx = static_cast<size_t>(raw_fd);

    auto* const proc = Kernel::Process::current_process();
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        const auto& desc = proc->fd_table[fd_idx];
        if (!desc.in_use) {
            return ERR_EBADF;
        }
        if (!desc.is_tty()) {
            return ERR_ENOTTY;
        }
        root_paddr = proc->address_space.root_paddr();
    } else {
        if (fd_idx > 2U) {
            return ERR_EBADF;
        }
        root_paddr = Kernel::MM::AddressSpace::current_root_paddr();
    }

    switch (request) {
    case IOCTL_TCGETS:
        return Kernel::MM::copy_to_user(root_paddr, arg, &g_console_termios,
                                        sizeof(g_console_termios));

    case IOCTL_TCSETS:
    case IOCTL_TCSETSW:
    case IOCTL_TCSETSF: {
        ktermios updated{};
        const int64_t copy_res =
            Kernel::MM::copy_from_user(root_paddr, &updated, arg, sizeof(updated));
        if (copy_res < 0) {
            return copy_res;
        }
        g_console_termios = updated;
        return 0;
    }

    case IOCTL_TIOCGWINSZ: {
        constexpr winsize default_ws{
            .ws_row = DEFAULT_TTY_ROWS,
            .ws_col = DEFAULT_TTY_COLS,
            .ws_xpixel = 0U,
            .ws_ypixel = 0U,
        };
        return Kernel::MM::copy_to_user(root_paddr, arg, &default_ws, sizeof(default_ws));
    }

    default:
        return ERR_EINVAL;
    }
}

int64_t sys_read(uint64_t raw_fd, uintptr_t user_buf, uint64_t raw_count) noexcept {
    if (raw_fd >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }
    const auto fd_idx = static_cast<size_t>(raw_fd);

    auto* const proc = Kernel::Process::current_process();
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        const auto& desc = proc->fd_table[fd_idx];
        if (!desc.in_use || !desc.is_readable()) {
            return ERR_EBADF;
        }
        if (desc.type != Kernel::Process::FdType::ConsoleTty) {
            return ERR_EBADF;
        }
        root_paddr = proc->address_space.root_paddr();
    } else {
        if (fd_idx != 0U) {
            return ERR_EBADF;
        }
        root_paddr = Kernel::MM::AddressSpace::current_root_paddr();
    }

    if (raw_count > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }
    const auto count = static_cast<size_t>(raw_count);

    if (!validate_user_write_range(root_paddr, user_buf, count)) {
        return ERR_EFAULT;
    }
    if (count == 0U) {
        return 0;
    }

    const bool canonical = (g_console_termios.c_lflag & TERMIOS_ICANON) != 0U;
    const bool echo = (g_console_termios.c_lflag & TERMIOS_ECHO) != 0U;
    const bool echoe = (g_console_termios.c_lflag & TERMIOS_ECHOE) != 0U;
    const bool map_cr_to_nl = (g_console_termios.c_iflag & TERMIOS_ICRNL) != 0U;

    char kbuf[READ_CHUNK_SIZE]{};
    size_t total_read = 0U;
    size_t chunk_pos = 0U;
    bool line_done = false;

    while ((total_read + chunk_pos) < count && !line_done) {
        char chr = static_cast<char>(HAL::get_console().getc());

        if (canonical) {
            if (chr == '\b' || static_cast<uint8_t>(chr) == 0x7FU) {
                if (chunk_pos > 0U) {
                    chunk_pos--;
                    if (echo) {
                        if (echoe) {
                            HAL::get_console().write("\b \b", 3U);
                        } else {
                            HAL::get_console().putc('\b');
                        }
                    }
                }
                continue;
            }

            if (chr == '\r' && map_cr_to_nl) {
                chr = '\n';
            }

            kbuf[chunk_pos++] = chr;

            if (chr == '\n') {
                if (echo) {
                    HAL::get_console().write("\r\n", 2U);
                }
                line_done = true;
            } else if (echo) {
                const auto byte_val = static_cast<uint8_t>(chr);
                if (byte_val >= 0x20U && byte_val <= 0x7EU) {
                    HAL::get_console().putc(chr);
                }
            }
        } else {
            if (chr == '\r' && map_cr_to_nl) {
                chr = '\n';
            }
            kbuf[chunk_pos++] = chr;
            if (echo) {
                HAL::get_console().putc(chr);
            }
        }

        if (chunk_pos == READ_CHUNK_SIZE || (total_read + chunk_pos) == count || line_done) {
            const int64_t copy_res =
                Kernel::MM::copy_to_user(root_paddr, user_buf + total_read, kbuf, chunk_pos);
            if (copy_res < 0) {
                return (total_read > 0U) ? static_cast<int64_t>(total_read) : copy_res;
            }
            total_read += chunk_pos;
            chunk_pos = 0U;
        }
    }

    return static_cast<int64_t>(total_read);
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
        if (desc.type != Kernel::Process::FdType::ConsoleTty) {
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

int64_t sys_brk(uintptr_t addr) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return 0;
    }

    if (addr == 0U || addr < proc->brk_start) {
        return static_cast<int64_t>(proc->brk_current);
    }

    const auto platform = HAL::get_platform_info();
    const uintptr_t stack_floor =
        platform.user_stack_top - Kernel::Process::DEFAULT_USER_STACK_SIZE;
    uintptr_t brk_ceiling = (MMAP_ARENA_FLOOR < stack_floor) ? MMAP_ARENA_FLOOR : stack_floor;
    if (proc->mmap_next_vaddr < brk_ceiling) {
        brk_ceiling = proc->mmap_next_vaddr;
    }

    if (addr > brk_ceiling) {
        return static_cast<int64_t>(proc->brk_current);
    }

    const size_t page_size = platform.page_size;
    const uintptr_t page_mask = page_size - 1U;
    const uintptr_t old_page_end = (proc->brk_current + page_mask) & ~page_mask;
    const uintptr_t new_page_end = (addr + page_mask) & ~page_mask;

    if (new_page_end > old_page_end) {
        const HAL::PageFlags heap_flags =
            HAL::PageFlags::User | HAL::PageFlags::Read | HAL::PageFlags::Write;

        for (uintptr_t page_vaddr = old_page_end; page_vaddr < new_page_end;
             page_vaddr += page_size) {
            const uintptr_t paddr = alloc_frame();
            if (paddr == 0U) {
                unmap_and_free_range(proc->address_space, old_page_end, page_vaddr, page_size);
                return static_cast<int64_t>(proc->brk_current);
            }

            auto* const kva = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(paddr));
            for (size_t i = 0U; i < page_size; ++i) {
                kva[i] = 0U;
            }
            HAL::get_cpu().flush_dcache_range(reinterpret_cast<uintptr_t>(kva), page_size);

            if (!proc->address_space.map_user_page(page_vaddr, paddr, heap_flags)) {
                free_frame(paddr);
                unmap_and_free_range(proc->address_space, old_page_end, page_vaddr, page_size);
                return static_cast<int64_t>(proc->brk_current);
            }
        }
    } else if (new_page_end < old_page_end) {
        unmap_and_free_range(proc->address_space, new_page_end, old_page_end, page_size);
    }

    proc->brk_current = addr;
    return static_cast<int64_t>(proc->brk_current);
}

int64_t sys_mmap(uintptr_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t raw_fd,
                 uint64_t offset) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uintptr_t page_mask = page_size - 1U;

    if (length == 0U || (length & page_mask) != 0U) {
        return ERR_EINVAL;
    }
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0U) {
        return ERR_EINVAL;
    }
    if ((flags & MAP_ANONYMOUS) == 0U || (flags & MAP_PRIVATE) == 0U) {
        return ERR_EINVAL;
    }
    if ((flags & (MAP_SHARED | MAP_FIXED)) != 0U) {
        return ERR_EINVAL;
    }
    if (static_cast<int64_t>(raw_fd) != -1 || offset != 0U) {
        return ERR_EINVAL;
    }
    (void)addr;

    const uintptr_t arena_top = proc->mmap_next_vaddr;
    if (length > arena_top || (arena_top - static_cast<uintptr_t>(length)) < MMAP_ARENA_FLOOR) {
        return ERR_ENOMEM;
    }

    const uintptr_t map_start = arena_top - static_cast<uintptr_t>(length);
    if (map_start <= proc->brk_current) {
        return ERR_ENOMEM;
    }

    HAL::PageFlags page_flags = HAL::PageFlags::None;
    if (prot != PROT_NONE) {
        page_flags = HAL::PageFlags::User;
        if ((prot & PROT_READ) != 0U) {
            page_flags |= HAL::PageFlags::Read;
        }
        if ((prot & PROT_WRITE) != 0U) {
            page_flags |= HAL::PageFlags::Write;
        }
        if ((prot & PROT_EXEC) != 0U) {
            page_flags |= HAL::PageFlags::Execute;
        }
    }

    for (uintptr_t page_vaddr = map_start; page_vaddr < arena_top; page_vaddr += page_size) {
        const uintptr_t paddr = alloc_frame();
        if (paddr == 0U) {
            unmap_and_free_range(proc->address_space, map_start, page_vaddr, page_size);
            return ERR_ENOMEM;
        }

        auto* const kva = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(paddr));
        for (size_t i = 0U; i < page_size; ++i) {
            kva[i] = 0U;
        }
        HAL::get_cpu().flush_dcache_range(reinterpret_cast<uintptr_t>(kva), page_size);

        if (!proc->address_space.map_user_page(page_vaddr, paddr, page_flags)) {
            free_frame(paddr);
            unmap_and_free_range(proc->address_space, map_start, page_vaddr, page_size);
            return ERR_ENOMEM;
        }
    }

    if ((prot & PROT_EXEC) != 0U) {
        HAL::get_cpu().invalidate_icache();
    }

    proc->mmap_next_vaddr = map_start;
    return static_cast<int64_t>(map_start);
}

int64_t sys_munmap(uintptr_t addr, uint64_t length) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const auto platform = HAL::get_platform_info();
    const size_t page_size = platform.page_size;
    const uintptr_t page_mask = page_size - 1U;
    const uintptr_t va_limit = Kernel::MM::user_addr_space_limit();
    const uintptr_t stack_floor =
        platform.user_stack_top - Kernel::Process::DEFAULT_USER_STACK_SIZE;

    if ((addr & page_mask) != 0U || length == 0U || (length & page_mask) != 0U) {
        return ERR_EINVAL;
    }
    if (addr >= va_limit || length > (va_limit - addr)) {
        return ERR_EINVAL;
    }

    const uintptr_t end_addr = addr + static_cast<uintptr_t>(length);
    if (addr < MMAP_ARENA_FLOOR || end_addr > stack_floor) {
        return ERR_EINVAL;
    }

    unmap_and_free_range(proc->address_space, addr, end_addr, page_size);

    if (addr == proc->mmap_next_vaddr) {
        proc->mmap_next_vaddr = end_addr;
    }

    return 0;
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
        case SYS_IOCTL:
            ret_val = sys_ioctl(cpu.get_syscall_arg(ctx, 0U), cpu.get_syscall_arg(ctx, 1U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)));
            break;

        case SYS_READ:
            ret_val = sys_read(cpu.get_syscall_arg(ctx, 0U),
                               static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                               cpu.get_syscall_arg(ctx, 2U));
            break;

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

        case SYS_BRK:
            ret_val = sys_brk(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_MUNMAP:
            ret_val = sys_munmap(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                 cpu.get_syscall_arg(ctx, 1U));
            break;

        case SYS_MMAP:
            ret_val = sys_mmap(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                               cpu.get_syscall_arg(ctx, 1U), cpu.get_syscall_arg(ctx, 2U),
                               cpu.get_syscall_arg(ctx, 3U), cpu.get_syscall_arg(ctx, 4U),
                               cpu.get_syscall_arg(ctx, 5U));
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