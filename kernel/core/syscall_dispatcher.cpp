#include "boot/fdt.hpp"
#include "fs/cpio.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "hal/trap.hpp"
#include "loader/elf_loader.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"
#include "process/process.hpp"
#include "process/stack_builder.hpp"

#include <cstddef>
#include <cstdint>

extern FDT::InitrdLocation g_initrd_loc;

namespace {

// POSIX / Linux Generic Syscall Numbers
constexpr uint64_t SYS_IOCTL = 29U;
constexpr uint64_t SYS_READ = 63U;
constexpr uint64_t SYS_WRITE = 64U;
constexpr uint64_t SYS_WRITEV = 66U;
constexpr uint64_t SYS_EXIT = 93U;
constexpr uint64_t SYS_EXIT_GROUP = 94U;
constexpr uint64_t SYS_SET_TID_ADDRESS = 96U;
constexpr uint64_t SYS_GETPID = 172U;
constexpr uint64_t SYS_GETPPID = 173U;
constexpr uint64_t SYS_BRK = 214U;
constexpr uint64_t SYS_MUNMAP = 215U;
constexpr uint64_t SYS_CLONE = 220U;
constexpr uint64_t SYS_EXECVE = 221U;
constexpr uint64_t SYS_MMAP = 222U;
constexpr uint64_t SYS_MPROTECT = 226U;
constexpr uint64_t SYS_WAIT4 = 260U;

constexpr size_t UIO_MAXIOV = 1024U;

// Negated POSIX Error Codes
constexpr int64_t ERR_ENOENT = -2;
constexpr int64_t ERR_E2BIG = -7;
constexpr int64_t ERR_ENOEXEC = -8;
constexpr int64_t ERR_EBADF = -9;
constexpr int64_t ERR_ECHILD = -10;
constexpr int64_t ERR_EAGAIN = -11;
constexpr int64_t ERR_ENOMEM = -12;
constexpr int64_t ERR_EFAULT = -14;
constexpr int64_t ERR_EINVAL = -22;
constexpr int64_t ERR_ENOTTY = -25;
constexpr int64_t ERR_ENOSYS = -38;

// Linux clone() Flags
constexpr uint64_t CLONE_VM = 0x00000100ULL;
constexpr uint64_t CLONE_FS = 0x00000200ULL;
constexpr uint64_t CLONE_FILES = 0x00000400ULL;
constexpr uint64_t CLONE_SIGHAND = 0x00000800ULL;
constexpr uint64_t CLONE_VFORK = 0x00004000ULL;
constexpr uint64_t CLONE_THREAD = 0x00010000ULL;
constexpr uint64_t CLONE_PARENT_SETTID = 0x00100000ULL;
constexpr uint64_t CLONE_CHILD_CLEARTID = 0x00200000ULL;
constexpr uint64_t CLONE_CHILD_SETTID = 0x01000000ULL;
constexpr uint64_t CSIGNAL_MASK = 0x000000FFULL;
constexpr uint64_t SIGCHLD_NUM = 17ULL;

// Linux wait4() Options
constexpr uint64_t WAIT_WNOHANG = 0x00000001ULL;
constexpr uint64_t WAIT_WUNTRACED = 0x00000002ULL;
constexpr uint64_t WAIT_WCONTINUED = 0x00000008ULL;
constexpr size_t LINUX_RUSAGE_SIZE = 144U;

// Bounded kernel staging limits for sys_execve (backed by a single 4 KiB frame)
constexpr size_t EXEC_MAX_PATH_LEN = 256U;
constexpr size_t EXEC_MAX_ARGS = 32U;
constexpr size_t EXEC_MAX_ENVS = 32U;

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

struct UserIovec {
    uint64_t iov_base;
    uint64_t iov_len;
};
static_assert(sizeof(UserIovec) == 16U, "UserIovec must match Linux struct iovec ABI");

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

int64_t copy_user_cstring(uintptr_t root_paddr, uintptr_t user_str, char* dst, size_t max_len,
                          size_t& out_len) noexcept {
    if (user_str == 0U || dst == nullptr || max_len == 0U) {
        return ERR_EFAULT;
    }

    for (size_t idx = 0U; idx < max_len; ++idx) {
        char chr = '\0';
        const int64_t res = Kernel::MM::copy_from_user(root_paddr, &chr, user_str + idx, 1U);
        if (res < 0) {
            return ERR_EFAULT;
        }
        dst[idx] = chr;
        if (chr == '\0') {
            out_len = idx;
            return 0;
        }
    }

    return ERR_E2BIG;
}

int64_t stage_user_string_array(uintptr_t root_paddr, uintptr_t user_array, const char** out_ptrs,
                                size_t max_entries, char* pool, size_t pool_cap,
                                size_t& pool_offset, int& out_count) noexcept {
    out_count = 0;
    out_ptrs[0] = nullptr;
    if (user_array == 0U) {
        return 0;
    }

    for (size_t idx = 0U; idx <= max_entries; ++idx) {
        uint64_t user_str_ptr = 0ULL;
        const uintptr_t elem_vaddr = user_array + (idx * sizeof(uint64_t));
        const int64_t ptr_res =
            Kernel::MM::copy_from_user(root_paddr, &user_str_ptr, elem_vaddr, sizeof(uint64_t));
        if (ptr_res < 0) {
            return ERR_EFAULT;
        }

        if (user_str_ptr == 0ULL) {
            out_ptrs[idx] = nullptr;
            out_count = static_cast<int>(idx);
            return 0;
        }

        if (idx == max_entries || pool_offset >= pool_cap) {
            return ERR_E2BIG;
        }

        size_t str_len = 0U;
        const int64_t str_res =
            copy_user_cstring(root_paddr, static_cast<uintptr_t>(user_str_ptr), pool + pool_offset,
                              pool_cap - pool_offset, str_len);
        if (str_res < 0) {
            return str_res;
        }

        out_ptrs[idx] = pool + pool_offset;
        pool_offset += str_len + 1U;
    }

    return ERR_E2BIG;
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

[[noreturn]] void sys_exit(uint64_t raw_status) noexcept {
    const auto exit_code =
        static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(raw_status)));
    auto* const proc = Kernel::Process::current_process();
    if (proc != nullptr) {
        proc->exit_status = exit_code;
        proc->state = Kernel::Process::ProcessState::Zombie;
    }
    HAL::get_cpu().return_to_kernel(exit_code);
    while (true) {
        HAL::get_cpu().halt();
    }
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

int64_t sys_clone(HAL::CpuContext& parent_ctx, uint64_t flags, uintptr_t child_stack,
                  uintptr_t parent_tid, uint64_t tls, uintptr_t child_tid) noexcept {
    (void)tls;

    auto* const parent = Kernel::Process::current_process();
    if (parent == nullptr) {
        return ERR_EINVAL;
    }

    constexpr uint64_t UNSUPPORTED_CLONE_FLAGS =
        CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_VFORK | CLONE_THREAD;
    constexpr uint64_t ALLOWED_CLONE_FLAGS =
        CSIGNAL_MASK | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;

    if ((flags & UNSUPPORTED_CLONE_FLAGS) != 0ULL || (flags & ~ALLOWED_CLONE_FLAGS) != 0ULL) {
        return ERR_EINVAL;
    }

    const uint64_t exit_sig = flags & CSIGNAL_MASK;
    if (exit_sig != 0ULL && exit_sig != SIGCHLD_NUM) {
        return ERR_EINVAL;
    }

    if (child_stack != 0U && child_stack >= Kernel::MM::user_addr_space_limit()) {
        return ERR_EINVAL;
    }

    auto* const child = Kernel::Process::allocate_process(parent->pid);
    if (child == nullptr) {
        return ERR_EAGAIN;
    }

    auto clone_as_res = parent->address_space.clone();
    if (!clone_as_res.is_ok()) {
        Kernel::Process::release_process(*child);
        return ERR_ENOMEM;
    }

    child->address_space = static_cast<Kernel::MM::AddressSpace&&>(clone_as_res.value());
    child->entry_point = parent->entry_point;
    child->brk_start = parent->brk_start;
    child->brk_current = parent->brk_current;
    child->mmap_next_vaddr = parent->mmap_next_vaddr;
    child->user_stack = Kernel::Process::UserStack{};

    for (size_t idx = 0U; idx < Kernel::Process::MAX_FDS_PER_PROCESS; ++idx) {
        child->fd_table[idx] = parent->fd_table[idx];
    }

    const auto child_pid_s32 = static_cast<int32_t>(child->pid);
    if ((flags & CLONE_PARENT_SETTID) != 0ULL && parent_tid != 0U) {
        const int64_t tid_res = Kernel::MM::copy_to_user(parent->address_space, parent_tid,
                                                         &child_pid_s32, sizeof(child_pid_s32));
        if (tid_res < 0) {
            Kernel::Process::release_process(*child);
            return ERR_EFAULT;
        }
    }

    if ((flags & CLONE_CHILD_SETTID) != 0ULL && child_tid != 0U) {
        const int64_t tid_res = Kernel::MM::copy_to_user(child->address_space, child_tid,
                                                         &child_pid_s32, sizeof(child_pid_s32));
        if (tid_res < 0) {
            Kernel::Process::release_process(*child);
            return ERR_EFAULT;
        }
    }

    auto& cpu = HAL::get_cpu();
    child->context = parent_ctx;
    if (child_stack != 0U) {
        cpu.set_user_stack_pointer(child->context, child_stack);
    }
    cpu.set_syscall_return(child->context, 0);
    child->state = Kernel::Process::ProcessState::Ready;

    return static_cast<int64_t>(child->pid);
}

int64_t sys_execve(HAL::CpuContext& ctx, uintptr_t user_pathname, uintptr_t user_argv,
                   uintptr_t user_envp) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uintptr_t scratch_paddr = alloc_frame();
    if (scratch_paddr == 0U) {
        return ERR_ENOMEM;
    }

    auto* const scratch_kva = reinterpret_cast<char*>(Kernel::MM::phys_to_virt(scratch_paddr));
    char* const path_buf = scratch_kva;
    char* const string_pool = scratch_kva + EXEC_MAX_PATH_LEN;
    const size_t string_pool_cap = page_size - EXEC_MAX_PATH_LEN;

    const char* argv_kptrs[EXEC_MAX_ARGS + 1U]{};
    const char* envp_kptrs[EXEC_MAX_ENVS + 1U]{};

    const uintptr_t caller_root = proc->address_space.root_paddr();
    size_t path_len = 0U;
    const int64_t path_res =
        copy_user_cstring(caller_root, user_pathname, path_buf, EXEC_MAX_PATH_LEN, path_len);
    if (path_res < 0) {
        free_frame(scratch_paddr);
        return path_res;
    }

    size_t pool_offset = 0U;
    int argc = 0;
    const int64_t argv_res =
        stage_user_string_array(caller_root, user_argv, argv_kptrs, EXEC_MAX_ARGS, string_pool,
                                string_pool_cap, pool_offset, argc);
    if (argv_res < 0) {
        free_frame(scratch_paddr);
        return argv_res;
    }

    int envc = 0;
    const int64_t envp_res =
        stage_user_string_array(caller_root, user_envp, envp_kptrs, EXEC_MAX_ENVS, string_pool,
                                string_pool_cap, pool_offset, envc);
    if (envp_res < 0) {
        free_frame(scratch_paddr);
        return envp_res;
    }
    (void)envc;

    if (!g_initrd_loc.valid || g_initrd_loc.start_paddr == 0U || g_initrd_loc.size == 0U) {
        free_frame(scratch_paddr);
        return ERR_ENOENT;
    }

    CpioEntry entry{};
    const auto* const initrd_kva =
        reinterpret_cast<const void*>(Kernel::MM::phys_to_virt(g_initrd_loc.start_paddr));
    const CpioStatus cpio_st = cpio_lookup(initrd_kva, g_initrd_loc.size, path_buf, entry);
    if (cpio_st != CpioStatus::Success) {
        free_frame(scratch_paddr);
        return ERR_ENOENT;
    }
    if (!entry.is_regular_file || entry.data == nullptr || entry.size == 0U) {
        free_frame(scratch_paddr);
        return ERR_ENOEXEC;
    }

    auto load_res = Kernel::Loader::load_elf(entry.data, entry.size);
    if (!load_res.is_ok()) {
        free_frame(scratch_paddr);
        return (load_res.error() == Kernel::Loader::ElfLoaderError::OutOfMemory) ? ERR_ENOMEM
                                                                                 : ERR_ENOEXEC;
    }

    auto loaded = static_cast<Kernel::Loader::LoadedElf&&>(load_res.value());
    auto stack_res = Kernel::Process::UserStackBuilder::create_and_map(loaded.address_space);
    if (!stack_res.is_ok()) {
        loaded.address_space.destroy();
        free_frame(scratch_paddr);
        return ERR_ENOMEM;
    }

    auto new_stack = stack_res.value();
    Elf64_auxv_t auxv[Kernel::Process::STANDARD_AUXV_COUNT]{};
    const size_t auxv_count =
        Kernel::Process::populate_standard_auxv(loaded, auxv, Kernel::Process::STANDARD_AUXV_COUNT);

    auto abi_res = Kernel::Process::UserStackBuilder::build_abi_layout(
        new_stack, argc, argv_kptrs, envp_kptrs, auxv, auxv_count);
    if (!abi_res.is_ok()) {
        free_frames(new_stack.phys_base, new_stack.num_pages);
        loaded.address_space.destroy();
        free_frame(scratch_paddr);
        return ERR_E2BIG;
    }

    // Switch TTBR0_EL1 to the new AddressSpace before tearing down the old one
    loaded.address_space.activate();

    if (proc->user_stack.phys_base != 0U && proc->user_stack.num_pages > 0U) {
        free_frames(proc->user_stack.phys_base, proc->user_stack.num_pages);
    }
    proc->address_space = static_cast<Kernel::MM::AddressSpace&&>(loaded.address_space);

    const auto platform = HAL::get_platform_info();
    proc->entry_point = loaded.entry_point;
    proc->user_stack = new_stack;
    const uintptr_t new_brk = (loaded.max_vaddr != 0U) ? loaded.max_vaddr : platform.user_brk_base;
    proc->brk_start = new_brk;
    proc->brk_current = new_brk;
    proc->mmap_next_vaddr = platform.user_mmap_base;

    Kernel::Process::init_user_cpu_context(ctx, proc->entry_point, proc->user_stack.final_sp_el0,
                                           proc->user_stack.argc, proc->user_stack.argv_vaddr);

    free_frame(scratch_paddr);
    return 0;
}

int64_t sys_wait4(int32_t target_pid, uintptr_t user_wstatus, uint64_t options,
                  uintptr_t user_rusage) noexcept {
    auto* const parent = Kernel::Process::current_process();
    if (parent == nullptr) {
        return ERR_EINVAL;
    }

    constexpr uint64_t ALLOWED_WAIT_OPTIONS = WAIT_WNOHANG | WAIT_WUNTRACED | WAIT_WCONTINUED;
    if ((options & ~ALLOWED_WAIT_OPTIONS) != 0ULL) {
        return ERR_EINVAL;
    }

    if (target_pid == 0 || target_pid < -1) {
        return ERR_ECHILD;
    }

    Kernel::Process::Process* zombie_child = nullptr;
    Kernel::Process::Process* ready_child = nullptr;

    for (size_t idx = 0U; idx < Kernel::Process::MAX_PROCESSES; ++idx) {
        auto* const candidate = Kernel::Process::process_at(idx);
        if (candidate == nullptr || candidate->state == Kernel::Process::ProcessState::Unused) {
            continue;
        }
        if (candidate->ppid != parent->pid) {
            continue;
        }
        if (target_pid > 0 && candidate->pid != static_cast<uint32_t>(target_pid)) {
            continue;
        }

        if (candidate->state == Kernel::Process::ProcessState::Zombie) {
            zombie_child = candidate;
            break;
        }
        if (candidate->state == Kernel::Process::ProcessState::Ready && ready_child == nullptr) {
            ready_child = candidate;
        }
    }

    Kernel::Process::Process* target_child = zombie_child;
    if (target_child == nullptr) {
        if (ready_child == nullptr) {
            return ERR_ECHILD;
        }
        if ((options & WAIT_WNOHANG) != 0ULL) {
            return 0;
        }

        parent->state = Kernel::Process::ProcessState::Blocked;
        const int64_t child_ret = Kernel::Process::execute_process(*ready_child);
        parent->state = Kernel::Process::ProcessState::Running;

        if (ready_child->state != Kernel::Process::ProcessState::Zombie) {
            ready_child->exit_status = child_ret;
            ready_child->state = Kernel::Process::ProcessState::Zombie;
        }
        target_child = ready_child;
    }

    if (user_wstatus != 0U) {
        const auto encoded_wstatus =
            static_cast<int32_t>((static_cast<uint32_t>(target_child->exit_status) & 0xFFU) << 8U);
        const int64_t status_res = Kernel::MM::copy_to_user(
            parent->address_space, user_wstatus, &encoded_wstatus, sizeof(encoded_wstatus));
        if (status_res < 0) {
            return ERR_EFAULT;
        }
    }

    if (user_rusage != 0U) {
        uint8_t zero_rusage[LINUX_RUSAGE_SIZE]{};
        const int64_t rusage_res = Kernel::MM::copy_to_user(parent->address_space, user_rusage,
                                                            zero_rusage, sizeof(zero_rusage));
        if (rusage_res < 0) {
            return ERR_EFAULT;
        }
    }

    const uint32_t reaped_pid = target_child->pid;
    Kernel::Process::release_process(*target_child);
    return static_cast<int64_t>(reaped_pid);
}

int64_t sys_set_tid_address(uintptr_t tidptr) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return 0;
    }
    proc->clear_child_tid = tidptr;
    return static_cast<int64_t>(proc->pid);
}

int64_t sys_writev(uint64_t raw_fd, uintptr_t user_iov, uint64_t raw_iovcnt) noexcept {
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

    if (raw_iovcnt == 0U) {
        return 0;
    }
    if (raw_iovcnt > UIO_MAXIOV) {
        return ERR_EINVAL;
    }
    const auto iovcnt = static_cast<size_t>(raw_iovcnt);

    if (!validate_user_read_range(root_paddr, user_iov, iovcnt * sizeof(UserIovec))) {
        return ERR_EFAULT;
    }

    char kbuf[WRITE_CHUNK_SIZE]{};
    uint64_t total_written = 0ULL;

    for (size_t idx = 0U; idx < iovcnt; ++idx) {
        UserIovec iov{};
        const uintptr_t entry_vaddr = user_iov + (idx * sizeof(UserIovec));
        const int64_t iov_res =
            Kernel::MM::copy_from_user(root_paddr, &iov, entry_vaddr, sizeof(UserIovec));
        if (iov_res < 0) {
            return (total_written > 0ULL) ? static_cast<int64_t>(total_written) : iov_res;
        }

        if (iov.iov_len == 0ULL) {
            continue;
        }
        if (iov.iov_len > (UINT64_MAX - total_written) ||
            (total_written + iov.iov_len) > static_cast<uint64_t>(INT64_MAX)) {
            return (total_written > 0ULL) ? static_cast<int64_t>(total_written) : ERR_EINVAL;
        }

        const auto buf_vaddr = static_cast<uintptr_t>(iov.iov_base);
        const auto buf_len = static_cast<size_t>(iov.iov_len);
        if (!validate_user_read_range(root_paddr, buf_vaddr, buf_len)) {
            return (total_written > 0ULL) ? static_cast<int64_t>(total_written) : ERR_EFAULT;
        }

        size_t vec_written = 0U;
        while (vec_written < buf_len) {
            const size_t remaining = buf_len - vec_written;
            const size_t chunk = (remaining < WRITE_CHUNK_SIZE) ? remaining : WRITE_CHUNK_SIZE;

            const int64_t copy_res =
                Kernel::MM::copy_from_user(root_paddr, kbuf, buf_vaddr + vec_written, chunk);
            if (copy_res < 0) {
                return (total_written > 0ULL) ? static_cast<int64_t>(total_written) : copy_res;
            }

            HAL::get_console().write(kbuf, chunk);
            vec_written += chunk;
            total_written += chunk;
        }
    }

    return static_cast<int64_t>(total_written);
}

int64_t sys_mprotect(uintptr_t addr, uint64_t length, uint64_t prot) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uintptr_t page_mask = page_size - 1U;
    const uintptr_t va_limit = Kernel::MM::user_addr_space_limit();

    if ((addr & page_mask) != 0U) {
        return ERR_EINVAL;
    }
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0U) {
        return ERR_EINVAL;
    }
    if (addr >= va_limit) {
        return ERR_ENOMEM;
    }
    if (length == 0U) {
        return 0;
    }
    if (length > (UINT64_MAX - page_mask)) {
        return ERR_ENOMEM;
    }

    const uint64_t aligned_len = (length + page_mask) & ~page_mask;
    if (aligned_len > (va_limit - addr)) {
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

    const uintptr_t end_addr = addr + static_cast<uintptr_t>(aligned_len);
    for (uintptr_t page_vaddr = addr; page_vaddr < end_addr; page_vaddr += page_size) {
        if (!proc->address_space.protect_user_page(page_vaddr, page_flags)) {
            return ERR_ENOMEM;
        }
    }

    if ((prot & PROT_EXEC) != 0U) {
        HAL::get_cpu().invalidate_icache();
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
            sys_exit(cpu.get_syscall_arg(ctx, 0U));

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

        case SYS_CLONE:
            ret_val = sys_clone(ctx, cpu.get_syscall_arg(ctx, 0U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)),
                                cpu.get_syscall_arg(ctx, 3U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 4U)));
            break;

        case SYS_EXECVE:
            ret_val = sys_execve(ctx, static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                 static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                 static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)));
            if (ret_val == 0) {
                return; // Preserve argc in ctx.x[0] initialized for the new binary
            }
            break;

        case SYS_WAIT4:
            ret_val = sys_wait4(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)));
            break;

        case SYS_WRITEV:
            ret_val = sys_writev(cpu.get_syscall_arg(ctx, 0U),
                                 static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                 cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_SET_TID_ADDRESS:
            ret_val = sys_set_tid_address(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_MPROTECT:
            ret_val = sys_mprotect(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                   cpu.get_syscall_arg(ctx, 1U), cpu.get_syscall_arg(ctx, 2U));
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