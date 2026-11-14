#include "boot/fdt.hpp"
#include "fs/cpio.hpp"
#include "fs/pipe.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "hal/trap.hpp"
#include "loader/elf_loader.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"
#include "process/process.hpp"
#include "process/scheduler.hpp"
#include "process/stack_builder.hpp"

#include <cstddef>
#include <cstdint>

extern FDT::InitrdLocation g_initrd_loc;

namespace {

// Linux AArch64 Syscall Numbers
constexpr uint64_t SYS_GETCWD = 17U;
constexpr uint64_t SYS_DUP = 23U;
constexpr uint64_t SYS_DUP3 = 24U;
constexpr uint64_t SYS_FCNTL = 25U;
constexpr uint64_t SYS_IOCTL = 29U;
constexpr uint64_t SYS_FACCESSAT = 48U;
constexpr uint64_t SYS_CHDIR = 49U;
constexpr uint64_t SYS_OPENAT = 56U;
constexpr uint64_t SYS_CLOSE = 57U;
constexpr uint64_t SYS_PIPE2 = 59U;
constexpr uint64_t SYS_GETDENTS64 = 61U;
constexpr uint64_t SYS_LSEEK = 62U;
constexpr uint64_t SYS_READ = 63U;
constexpr uint64_t SYS_WRITE = 64U;
constexpr uint64_t SYS_WRITEV = 66U;
constexpr uint64_t SYS_READLINKAT = 78U;
constexpr uint64_t SYS_NEWFSTATAT = 79U;
constexpr uint64_t SYS_FSTAT = 80U;
constexpr uint64_t SYS_EXIT = 93U;
constexpr uint64_t SYS_EXIT_GROUP = 94U;
constexpr uint64_t SYS_SET_TID_ADDRESS = 96U;
constexpr uint64_t SYS_CLOCK_GETTIME = 113U;
constexpr uint64_t SYS_SCHED_YIELD = 124U;
constexpr uint64_t SYS_KILL = 129U;
constexpr uint64_t SYS_RT_SIGACTION = 134U;
constexpr uint64_t SYS_RT_SIGPROCMASK = 135U;
constexpr uint64_t SYS_RT_SIGRETURN = 139U;
constexpr uint64_t SYS_SETPGID = 154U;
constexpr uint64_t SYS_GETPGID = 155U;
constexpr uint64_t SYS_GETSID = 156U;
constexpr uint64_t SYS_SETSID = 157U;
constexpr uint64_t SYS_UNAME = 160U;
constexpr uint64_t SYS_GETPID = 172U;
constexpr uint64_t SYS_GETPPID = 173U;
constexpr uint64_t SYS_GETUID = 174U;
constexpr uint64_t SYS_GETEUID = 175U;
constexpr uint64_t SYS_GETGID = 176U;
constexpr uint64_t SYS_GETEGID = 177U;
constexpr uint64_t SYS_SYSINFO = 179U;
constexpr uint64_t SYS_BRK = 214U;
constexpr uint64_t SYS_MUNMAP = 215U;
constexpr uint64_t SYS_CLONE = 220U;
constexpr uint64_t SYS_EXECVE = 221U;
constexpr uint64_t SYS_MMAP = 222U;
constexpr uint64_t SYS_MPROTECT = 226U;
constexpr uint64_t SYS_WAIT4 = 260U;

constexpr size_t UIO_MAXIOV = 1024U;

// Negated POSIX Error Codes
constexpr int64_t ERR_EPERM = -1;
constexpr int64_t ERR_ENOENT = -2;
constexpr int64_t ERR_ESRCH = -3;
constexpr int64_t ERR_E2BIG = -7;
constexpr int64_t ERR_ENOEXEC = -8;
constexpr int64_t ERR_EBADF = -9;
constexpr int64_t ERR_ECHILD = -10;
constexpr int64_t ERR_EAGAIN = -11;
constexpr int64_t ERR_ENOMEM = -12;
constexpr int64_t ERR_EACCES = -13;
constexpr int64_t ERR_EFAULT = -14;
constexpr int64_t ERR_ENOTDIR = -20;
constexpr int64_t ERR_EISDIR = -21;
constexpr int64_t ERR_EINVAL = -22;
constexpr int64_t ERR_EMFILE = -24;
constexpr int64_t ERR_ENOTTY = -25;
constexpr int64_t ERR_ESPIPE = -29;
constexpr int64_t ERR_EROFS = -30;
constexpr int64_t ERR_ERANGE = -34;
constexpr int64_t ERR_ENOSYS = -38;

// File / Open / Fcntl Constants
constexpr int32_t AT_FDCWD = -100;
constexpr uint64_t AT_EMPTY_PATH = 0x1000ULL;

constexpr uint64_t O_ACCMODE = 0x3ULL;
constexpr uint64_t O_RDONLY = 0x0ULL;
constexpr uint64_t O_WRONLY = 0x1ULL;
constexpr uint64_t O_RDWR = 0x2ULL;
constexpr uint64_t O_CREAT = 0x40ULL;
constexpr uint64_t O_TRUNC = 0x200ULL;
constexpr uint64_t O_DIRECTORY = 0x4000ULL; // AArch64 O_DIRECTORY = 040000
constexpr uint64_t O_NONBLOCK = 0x800ULL;
constexpr uint64_t O_CLOEXEC = 0x80000ULL;

constexpr uint64_t F_DUPFD = 0ULL;
constexpr uint64_t F_GETFD = 1ULL;
constexpr uint64_t F_SETFD = 2ULL;
constexpr uint64_t F_GETFL = 3ULL;
constexpr uint64_t F_SETFL = 4ULL;
constexpr uint64_t F_DUPFD_CLOEXEC = 1030ULL;
constexpr uint64_t FD_CLOEXEC_BIT = 1ULL;

constexpr uint64_t SEEK_SET = 0ULL;
constexpr uint64_t SEEK_CUR = 1ULL;
constexpr uint64_t SEEK_END = 2ULL;

constexpr uint8_t DT_DIR = 4U;
constexpr uint8_t DT_REG = 8U;
constexpr uint8_t DT_LNK = 10U;

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

constexpr size_t EXEC_MAX_PATH_LEN = 256U;
constexpr size_t EXEC_MAX_ARGS = 32U;
constexpr size_t EXEC_MAX_ENVS = 32U;

// mmap Protection & Mapping Flags
constexpr uint64_t PROT_NONE = 0x0U;
constexpr uint64_t PROT_READ = 0x1U;
constexpr uint64_t PROT_WRITE = 0x2U;
constexpr uint64_t PROT_EXEC = 0x4U;

constexpr uint64_t MAP_SHARED = 0x01U;
constexpr uint64_t MAP_PRIVATE = 0x02U;
constexpr uint64_t MAP_FIXED = 0x10U;
constexpr uint64_t MAP_ANONYMOUS = 0x20U;

constexpr uintptr_t MMAP_ARENA_FLOOR = 0x0000000020000000ULL;

// Terminal ioctl Request Codes
constexpr uint64_t IOCTL_TCGETS = 0x5401U;
constexpr uint64_t IOCTL_TCSETS = 0x5402U;
constexpr uint64_t IOCTL_TCSETSW = 0x5403U;
constexpr uint64_t IOCTL_TCSETSF = 0x5404U;
constexpr uint64_t IOCTL_TIOCGPGRP = 0x540FU;
constexpr uint64_t IOCTL_TIOCSPGRP = 0x5410U;
constexpr uint64_t IOCTL_TIOCGWINSZ = 0x5413U;

// termios Flag Bits
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

// Signal masks
constexpr uint64_t SIG_BLOCK = 0ULL;
constexpr uint64_t SIG_UNBLOCK = 1ULL;
constexpr uint64_t SIG_SETMASK = 2ULL;

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

// Linux AArch64 struct stat (128 bytes)
struct LinuxStat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t __pad2;
    int64_t st_blocks;
    int64_t st_atime_sec;
    int64_t st_atime_nsec;
    int64_t st_mtime_sec;
    int64_t st_mtime_nsec;
    int64_t st_ctime_sec;
    int64_t st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
};
static_assert(sizeof(LinuxStat) == 128U, "LinuxStat must match Linux AArch64 ABI (128 bytes)");

struct [[gnu::packed]] LinuxDirent64Header {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
};

static_assert(sizeof(LinuxDirent64Header) == 19U, "LinuxDirent64Header must be 19 bytes");

struct LinuxSigAction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};
static_assert(sizeof(LinuxSigAction) == 32U, "LinuxSigAction must be 32 bytes");

struct LinuxUtsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};
static_assert(sizeof(LinuxUtsname) == 390U, "LinuxUtsname must be 390 bytes");

struct LinuxTimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
static_assert(sizeof(LinuxTimespec) == 16U, "LinuxTimespec must be 16 bytes");

struct LinuxSysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char _f[20 - 2 * sizeof(uint64_t) - sizeof(uint32_t)];
};
static_assert(sizeof(LinuxSysinfo) == 112U, "LinuxSysinfo must be 112 bytes");

ktermios g_console_termios{
    .c_iflag = TERMIOS_ICRNL,
    .c_oflag = TERMIOS_OPOST | TERMIOS_ONLCR,
    .c_cflag = TERMIOS_CS8 | TERMIOS_CREAD | TERMIOS_CLOCAL,
    .c_lflag = TERMIOS_ISIG | TERMIOS_ICANON | TERMIOS_ECHO | TERMIOS_ECHOE | TERMIOS_ECHOK,
    .c_line = 0U,
    .c_cc = {0x03U, 0x1CU, 0x7FU, 0x15U, 0x04U, 0x00U, 0x01U, 0x00U, 0x11U, 0x13U, 0x1AU, 0x00U,
             0x12U, 0x0FU, 0x17U, 0x16U, 0x00U, 0x00U, 0x00U},
};

uint32_t g_foreground_pgid = 1U;

size_t cstr_len(const char* str) noexcept {
    size_t len = 0U;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

void cstr_copy(char* dst, const char* src, size_t cap) noexcept {
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

bool cstr_eq(const char* lhs, const char* rhs) noexcept {
    while (*lhs != '\0' && *lhs == *rhs) {
        lhs++;
        rhs++;
    }
    return *lhs == *rhs;
}

// Normalizes a base directory + relative/absolute input path into a canonical "/a/b" path
bool normalize_vfs_path(const char* base_dir, const char* input, char* out,
                        size_t out_cap) noexcept {
    if (input == nullptr || out == nullptr || out_cap < 2U) {
        return false;
    }

    char combined[Kernel::Process::MAX_PATH_LEN * 2U]{};
    size_t pos = 0U;

    if (input[0] != '/') {
        const char* base = (base_dir != nullptr && base_dir[0] == '/') ? base_dir : "/";
        for (size_t i = 0U; base[i] != '\0'; ++i) {
            if (pos + 2U >= sizeof(combined)) {
                return false;
            }
            combined[pos++] = base[i];
        }
        if (pos > 0U && combined[pos - 1U] != '/') {
            combined[pos++] = '/';
        }
    }

    for (size_t i = 0U; input[i] != '\0'; ++i) {
        if (pos + 1U >= sizeof(combined)) {
            return false;
        }
        combined[pos++] = input[i];
    }
    combined[pos] = '\0';

    // Stack of component start offsets inside `out`
    size_t comp_starts[32]{};
    size_t comp_count = 0U;
    out[0] = '/';
    size_t out_len = 1U;

    size_t idx = 0U;
    while (combined[idx] != '\0') {
        while (combined[idx] == '/') {
            idx++;
        }
        if (combined[idx] == '\0') {
            break;
        }

        const size_t start = idx;
        while (combined[idx] != '\0' && combined[idx] != '/') {
            idx++;
        }
        const size_t len = idx - start;

        if (len == 1U && combined[start] == '.') {
            continue;
        }
        if (len == 2U && combined[start] == '.' && combined[start + 1U] == '.') {
            if (comp_count > 0U) {
                out_len = comp_starts[--comp_count];
                if (out_len == 0U) {
                    out[0] = '/';
                    out_len = 1U;
                }
            }
            continue;
        }

        if (comp_count >= 32U) {
            return false;
        }
        comp_starts[comp_count++] = out_len;

        if (out_len > 1U) {
            if (out_len + 1U >= out_cap) {
                return false;
            }
            out[out_len++] = '/';
        }
        if (out_len + len >= out_cap) {
            return false;
        }
        for (size_t k = 0U; k < len; ++k) {
            out[out_len++] = combined[start + k];
        }
    }

    out[out_len] = '\0';
    return true;
}

// Resolves a canonical path ("/" or "/bin/sh") against initrd CPIO entries,
// synthesizing virtual directories if intermediate components exist.
bool resolve_initrd_path(const char* canon_path, CpioEntry& out_entry) noexcept {
    if (canon_path == nullptr || canon_path[0] != '/') {
        return false;
    }

    if (canon_path[1] == '\0') {
        out_entry = CpioEntry{
            .name = ".",
            .name_len = 1U,
            .data = nullptr,
            .size = 0U,
            .mode = CPIO_MODE_DIR | 0755U,
            .is_directory = true,
            .is_regular_file = false,
        };
        return true;
    }

    if (!g_initrd_loc.valid || g_initrd_loc.start_paddr == 0U || g_initrd_loc.size == 0U) {
        return false;
    }

    const auto* const initrd_kva =
        reinterpret_cast<const void*>(Kernel::MM::phys_to_virt(g_initrd_loc.start_paddr));
    const CpioArchive archive(initrd_kva, g_initrd_loc.size);
    if (!archive.is_valid()) {
        return false;
    }

    if (archive.lookup(canon_path, out_entry) == CpioStatus::Success) {
        return true;
    }

    // Check if any entry has `target_rel/` as a prefix (implicit directory in CPIO)
    const char* const target_rel = canon_path + 1;
    const size_t target_len = cstr_len(target_rel);
    bool has_children = false;

    archive.for_each([&](const CpioEntry& entry) -> bool {
        const char* norm = cpio_normalize_path(entry.name);
        size_t idx = 0U;
        while (idx < target_len && norm[idx] == target_rel[idx]) {
            idx++;
        }
        if (idx == target_len && norm[idx] == '/') {
            has_children = true;
            return false;
        }
        return true;
    });

    if (has_children) {
        out_entry = CpioEntry{
            .name = canon_path,
            .name_len = cstr_len(canon_path),
            .data = nullptr,
            .size = 0U,
            .mode = CPIO_MODE_DIR | 0755U,
            .is_directory = true,
            .is_regular_file = false,
        };
        return true;
    }

    return false;
}

uint64_t compute_ino(const char* path) noexcept {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0U; path != nullptr && path[i] != '\0'; ++i) {
        hash ^= static_cast<uint8_t>(path[i]);
        hash *= 1099511628211ULL;
    }
    return (hash == 0ULL) ? 1ULL : hash;
}

void fill_stat_from_fd(const Kernel::Process::FileDescriptor& desc, LinuxStat& stat_out) noexcept {
    stat_out = LinuxStat{};
    stat_out.st_dev = 1ULL;
    stat_out.st_nlink = 1U;
    stat_out.st_blksize = 4096;

    if (desc.type == Kernel::Process::FdType::ConsoleTty) {
        stat_out.st_ino = 2ULL;
        stat_out.st_mode = 0020666U; // S_IFCHR | 0666
        stat_out.st_rdev = 0x0501ULL;
    } else if (desc.type == Kernel::Process::FdType::PipeRead ||
               desc.type == Kernel::Process::FdType::PipeWrite) {
        stat_out.st_ino = 3ULL;
        stat_out.st_mode = 0010600U; // S_IFIFO | 0600
    } else if (desc.type == Kernel::Process::FdType::InitrdFile) {
        stat_out.st_ino = compute_ino(desc.path);
        stat_out.st_mode = desc.cpio_entry.mode;
        stat_out.st_size = static_cast<int64_t>(desc.cpio_entry.size);
        stat_out.st_blocks = static_cast<int64_t>((desc.cpio_entry.size + 511U) / 512U);
    }
}

void fill_stat_from_cpio(const char* canon_path, const CpioEntry& entry,
                         LinuxStat& stat_out) noexcept {
    stat_out = LinuxStat{};
    stat_out.st_dev = 1ULL;
    stat_out.st_ino = compute_ino(canon_path);
    stat_out.st_mode = entry.mode;
    stat_out.st_nlink = entry.is_directory ? 2U : 1U;
    stat_out.st_size = static_cast<int64_t>(entry.size);
    stat_out.st_blksize = 4096;
    stat_out.st_blocks = static_cast<int64_t>((entry.size + 511U) / 512U);
}

int allocate_fd_slot(Kernel::Process::Process& proc, size_t min_fd = 0U) noexcept {
    for (size_t idx = min_fd; idx < Kernel::Process::MAX_FDS_PER_PROCESS; ++idx) {
        if (!proc.fd_table[idx].in_use) {
            return static_cast<int>(idx);
        }
    }
    return -1;
}

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

int64_t resolve_dirfd_base(Kernel::Process::Process& proc, int32_t dirfd,
                           const char*& out_base) noexcept {
    if (dirfd == AT_FDCWD) {
        out_base = proc.cwd;
        return 0;
    }
    if (dirfd < 0 || static_cast<size_t>(dirfd) >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }
    const auto& desc = proc.fd_table[static_cast<size_t>(dirfd)];
    if (!desc.in_use) {
        return ERR_EBADF;
    }
    if (desc.type != Kernel::Process::FdType::InitrdFile || !desc.cpio_entry.is_directory) {
        return ERR_ENOTDIR;
    }
    out_base = desc.path;
    return 0;
}

// ==========================================================================
// Initrd VFS, Directory, Pipe & Descriptor Syscalls
// ==========================================================================

int64_t sys_getcwd(uintptr_t user_buf, uint64_t size) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (size == 0U) {
        return ERR_EINVAL;
    }
    const size_t len = cstr_len(proc->cwd) + 1U;
    if (size < len) {
        return ERR_ERANGE;
    }
    const int64_t res = Kernel::MM::copy_to_user(proc->address_space, user_buf, proc->cwd, len);
    if (res < 0) {
        return ERR_EFAULT;
    }
    return static_cast<int64_t>(len);
}

int64_t sys_chdir(uintptr_t user_path) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space.root_paddr(), user_path,
                                               raw_path, sizeof(raw_path), len);
    if (copy_res < 0) {
        return copy_res;
    }

    char canon_path[Kernel::Process::MAX_PATH_LEN]{};
    if (!normalize_vfs_path(proc->cwd, raw_path, canon_path, sizeof(canon_path))) {
        return ERR_ENOENT;
    }

    CpioEntry entry{};
    if (!resolve_initrd_path(canon_path, entry)) {
        return ERR_ENOENT;
    }
    if (!entry.is_directory) {
        return ERR_ENOTDIR;
    }

    cstr_copy(proc->cwd, canon_path, sizeof(proc->cwd));
    return 0;
}

int64_t sys_openat(int32_t dirfd, uintptr_t user_pathname, uint64_t flags, uint64_t mode) noexcept {
    (void)mode;
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    if ((flags & (O_CREAT | O_TRUNC)) != 0ULL || (flags & O_ACCMODE) != O_RDONLY) {
        return ERR_EROFS;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space.root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    const char* base_dir = "/";
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_base(*proc, dirfd, base_dir);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    char canon_path[Kernel::Process::MAX_PATH_LEN]{};
    if (!normalize_vfs_path(base_dir, raw_path, canon_path, sizeof(canon_path))) {
        return ERR_ENOENT;
    }

    const int slot = allocate_fd_slot(*proc);
    if (slot < 0) {
        return ERR_EMFILE;
    }

    if (cstr_eq(canon_path, "/dev/console") || cstr_eq(canon_path, "/dev/tty")) {
        uint32_t fd_flags = Kernel::Process::FD_FLAG_READ | Kernel::Process::FD_FLAG_WRITE;
        if ((flags & O_CLOEXEC) != 0ULL) {
            fd_flags |= Kernel::Process::FD_FLAG_CLOEXEC;
        }
        proc->fd_table[slot] = Kernel::Process::FileDescriptor{
            .type = Kernel::Process::FdType::ConsoleTty,
            .flags = fd_flags,
            .offset = 0U,
            .in_use = true,
        };
        return slot;
    }

    CpioEntry entry{};
    if (!resolve_initrd_path(canon_path, entry)) {
        return ERR_ENOENT;
    }

    if ((flags & O_DIRECTORY) != 0ULL && !entry.is_directory) {
        return ERR_ENOTDIR;
    }

    uint32_t fd_flags = Kernel::Process::FD_FLAG_READ;
    if ((flags & O_CLOEXEC) != 0ULL) {
        fd_flags |= Kernel::Process::FD_FLAG_CLOEXEC;
    }

    auto& desc = proc->fd_table[slot];
    desc = Kernel::Process::FileDescriptor{
        .type = Kernel::Process::FdType::InitrdFile,
        .flags = fd_flags,
        .offset = 0U,
        .in_use = true,
        .cpio_entry = entry,
    };
    cstr_copy(desc.path, canon_path, sizeof(desc.path));
    return slot;
}

int64_t sys_close(uint64_t raw_fd) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EBADF;
    }
    if (raw_fd >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }
    const auto fd_idx = static_cast<size_t>(raw_fd);
    if (!proc->fd_table[fd_idx].in_use) {
        return ERR_EBADF;
    }
    Kernel::Process::close_fd(*proc, fd_idx);
    return 0;
}

int64_t sys_lseek(uint64_t raw_fd, int64_t offset, uint64_t whence) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || raw_fd >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }
    auto& desc = proc->fd_table[static_cast<size_t>(raw_fd)];
    if (!desc.in_use) {
        return ERR_EBADF;
    }
    if (desc.type == Kernel::Process::FdType::PipeRead ||
        desc.type == Kernel::Process::FdType::PipeWrite ||
        desc.type == Kernel::Process::FdType::ConsoleTty) {
        return ERR_ESPIPE;
    }

    int64_t base = 0;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = static_cast<int64_t>(desc.offset);
    } else if (whence == SEEK_END) {
        base = static_cast<int64_t>(desc.cpio_entry.size);
    } else {
        return ERR_EINVAL;
    }

    const int64_t new_pos = base + offset;
    if (new_pos < 0) {
        return ERR_EINVAL;
    }
    desc.offset = static_cast<size_t>(new_pos);
    return new_pos;
}

int64_t sys_fstat(uint64_t raw_fd, uintptr_t user_statbuf) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || raw_fd >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }
    const auto& desc = proc->fd_table[static_cast<size_t>(raw_fd)];
    if (!desc.in_use) {
        return ERR_EBADF;
    }

    LinuxStat stat{};
    fill_stat_from_fd(desc, stat);
    return Kernel::MM::copy_to_user(proc->address_space, user_statbuf, &stat, sizeof(stat));
}

int64_t sys_newfstatat(int32_t dirfd, uintptr_t user_pathname, uintptr_t user_statbuf,
                       uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space.root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    if (path_len == 0U && (flags & AT_EMPTY_PATH) != 0ULL) {
        if (dirfd < 0 || static_cast<size_t>(dirfd) >= Kernel::Process::MAX_FDS_PER_PROCESS) {
            return ERR_EBADF;
        }
        const auto& desc = proc->fd_table[static_cast<size_t>(dirfd)];
        if (!desc.in_use) {
            return ERR_EBADF;
        }
        LinuxStat stat{};
        fill_stat_from_fd(desc, stat);
        return Kernel::MM::copy_to_user(proc->address_space, user_statbuf, &stat, sizeof(stat));
    }

    const char* base_dir = "/";
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_base(*proc, dirfd, base_dir);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    char canon_path[Kernel::Process::MAX_PATH_LEN]{};
    if (!normalize_vfs_path(base_dir, raw_path, canon_path, sizeof(canon_path))) {
        return ERR_ENOENT;
    }

    if (cstr_eq(canon_path, "/dev/console") || cstr_eq(canon_path, "/dev/tty")) {
        LinuxStat stat{};
        stat.st_dev = 1ULL;
        stat.st_ino = 2ULL;
        stat.st_mode = 0020666U;
        stat.st_nlink = 1U;
        stat.st_rdev = 0x0501ULL;
        stat.st_blksize = 4096;
        return Kernel::MM::copy_to_user(proc->address_space, user_statbuf, &stat, sizeof(stat));
    }

    CpioEntry entry{};
    if (!resolve_initrd_path(canon_path, entry)) {
        return ERR_ENOENT;
    }

    LinuxStat stat{};
    fill_stat_from_cpio(canon_path, entry, stat);
    return Kernel::MM::copy_to_user(proc->address_space, user_statbuf, &stat, sizeof(stat));
}

int64_t sys_faccessat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                      uint64_t flags) noexcept {
    (void)flags;
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space.root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    const char* base_dir = "/";
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_base(*proc, dirfd, base_dir);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    char canon_path[Kernel::Process::MAX_PATH_LEN]{};
    if (!normalize_vfs_path(base_dir, raw_path, canon_path, sizeof(canon_path))) {
        return ERR_ENOENT;
    }

    if (cstr_eq(canon_path, "/dev/console") || cstr_eq(canon_path, "/dev/tty")) {
        return 0;
    }

    CpioEntry entry{};
    if (!resolve_initrd_path(canon_path, entry)) {
        return ERR_ENOENT;
    }

    // W_OK (2) on read-only initrd
    if ((mode & 2ULL) != 0ULL) {
        return ERR_EROFS;
    }
    // X_OK (1)
    if ((mode & 1ULL) != 0ULL && !entry.is_directory && (entry.mode & 0111U) == 0U) {
        return ERR_EACCES;
    }
    return 0;
}

int64_t sys_readlinkat(int32_t dirfd, uintptr_t user_pathname, uintptr_t user_buf,
                       uint64_t bufsiz) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (bufsiz == 0U) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space.root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    const char* base_dir = "/";
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_base(*proc, dirfd, base_dir);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    char canon_path[Kernel::Process::MAX_PATH_LEN]{};
    if (!normalize_vfs_path(base_dir, raw_path, canon_path, sizeof(canon_path))) {
        return ERR_ENOENT;
    }

    if (cstr_eq(canon_path, "/proc/self/exe")) {
        const char exe_target[] = "/bin/busybox";
        const size_t copy_len = (bufsiz < (sizeof(exe_target) - 1U)) ? static_cast<size_t>(bufsiz)
                                                                     : (sizeof(exe_target) - 1U);
        if (Kernel::MM::copy_to_user(proc->address_space, user_buf, exe_target, copy_len) < 0) {
            return ERR_EFAULT;
        }
        return static_cast<int64_t>(copy_len);
    }

    CpioEntry entry{};
    if (!resolve_initrd_path(canon_path, entry)) {
        return ERR_ENOENT;
    }

    if ((entry.mode & CPIO_MODE_FILE_TYPE_MASK) != CPIO_MODE_SYMLINK) {
        return ERR_EINVAL;
    }

    const size_t copy_len = (bufsiz < entry.size) ? static_cast<size_t>(bufsiz) : entry.size;
    if (copy_len > 0U && entry.data != nullptr) {
        if (Kernel::MM::copy_to_user(proc->address_space, user_buf, entry.data, copy_len) < 0) {
            return ERR_EFAULT;
        }
    }
    return static_cast<int64_t>(copy_len);
}

int64_t sys_getdents64(uint64_t raw_fd, uintptr_t user_dirp, uint64_t count) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || raw_fd >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }

    auto& desc = proc->fd_table[static_cast<size_t>(raw_fd)];
    if (!desc.in_use) {
        return ERR_EBADF;
    }
    if (desc.type != Kernel::Process::FdType::InitrdFile || !desc.cpio_entry.is_directory) {
        return ERR_ENOTDIR;
    }

    const uintptr_t root_paddr = proc->address_space.root_paddr();
    if (!validate_user_write_range(root_paddr, user_dirp, static_cast<size_t>(count))) {
        return ERR_EFAULT;
    }

    // Track emitted direct children to deduplicate synthesized parent directories
    char seen_names[64][64]{};
    size_t seen_count = 0U;
    size_t entry_index = 0U;
    size_t bytes_written = 0U;
    bool buffer_full = false;

    auto emit_dirent = [&](const char* name, uint8_t d_type) {
        if (buffer_full) {
            return;
        }
        for (size_t seen_idx = 0U; seen_idx < seen_count; ++seen_idx) {
            if (cstr_eq(seen_names[seen_idx], name)) {
                return;
            }
        }
        if (seen_count < 64U) {
            cstr_copy(seen_names[seen_count++], name, 64U);
        }

        const size_t cur_idx = entry_index++;
        if (cur_idx < desc.offset) {
            return;
        }

        const size_t name_len = cstr_len(name);
        const size_t raw_reclen = sizeof(LinuxDirent64Header) + name_len + 1U;
        const size_t aligned_reclen = (raw_reclen + 7U) & ~7U;

        if (bytes_written + aligned_reclen > count) {
            buffer_full = true;
            return;
        }

        uint8_t record[128]{};
        if (aligned_reclen > sizeof(record)) {
            return;
        }

        auto* const hdr = reinterpret_cast<LinuxDirent64Header*>(record);
        hdr->d_ino = compute_ino(name);
        hdr->d_off = static_cast<int64_t>(cur_idx + 1U);
        hdr->d_reclen = static_cast<uint16_t>(aligned_reclen);
        hdr->d_type = d_type;
        for (size_t k = 0U; k < name_len; ++k) {
            record[sizeof(LinuxDirent64Header) + k] = static_cast<uint8_t>(name[k]);
        }

        if (Kernel::MM::copy_to_user(root_paddr, user_dirp + bytes_written, record,
                                     aligned_reclen) < 0) {
            buffer_full = true;
            return;
        }

        bytes_written += aligned_reclen;
        desc.offset = cur_idx + 1U;
    };

    emit_dirent(".", DT_DIR);
    emit_dirent("..", DT_DIR);

    if (g_initrd_loc.valid && g_initrd_loc.start_paddr != 0U && g_initrd_loc.size != 0U) {
        const auto* const initrd_kva =
            reinterpret_cast<const void*>(Kernel::MM::phys_to_virt(g_initrd_loc.start_paddr));
        const CpioArchive archive(initrd_kva, g_initrd_loc.size);

        const char* const dir_rel = (desc.path[0] == '/') ? (desc.path + 1) : desc.path;
        const size_t dir_len = cstr_len(dir_rel);

        archive.for_each([&](const CpioEntry& entry) -> bool {
            if (buffer_full) {
                return false;
            }
            const char* norm = cpio_normalize_path(entry.name);
            if (norm[0] == '\0' || (norm[0] == '.' && norm[1] == '\0')) {
                return true;
            }

            const char* child_start = nullptr;
            if (dir_len == 0U) {
                child_start = norm;
            } else {
                size_t seen_idx = 0U;
                while (seen_idx < dir_len && norm[seen_idx] == dir_rel[seen_idx]) {
                    seen_idx++;
                }
                if (seen_idx != dir_len || norm[seen_idx] != '/' || norm[seen_idx + 1U] == '\0') {
                    return true;
                }
                child_start = norm + seen_idx + 1U;
            }

            char child_name[64]{};
            size_t c_len = 0U;
            bool has_subslash = false;
            while (child_start[c_len] != '\0' && child_start[c_len] != '/') {
                if (c_len + 1U < sizeof(child_name)) {
                    child_name[c_len] = child_start[c_len];
                }
                c_len++;
            }
            if (c_len == 0U || c_len >= sizeof(child_name)) {
                return true;
            }
            child_name[c_len] = '\0';
            if (child_start[c_len] == '/') {
                has_subslash = true;
            }

            uint8_t dtype = DT_REG;
            if (has_subslash || entry.is_directory) {
                dtype = DT_DIR;
            } else if ((entry.mode & CPIO_MODE_FILE_TYPE_MASK) == CPIO_MODE_SYMLINK) {
                dtype = DT_LNK;
            }

            emit_dirent(child_name, dtype);
            return !buffer_full;
        });
    }

    return static_cast<int64_t>(bytes_written);
}

int64_t sys_dup(uint64_t raw_oldfd) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || raw_oldfd >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }
    const auto old_idx = static_cast<size_t>(raw_oldfd);
    if (!proc->fd_table[old_idx].in_use) {
        return ERR_EBADF;
    }

    const int new_idx = allocate_fd_slot(*proc, 0U);
    if (new_idx < 0) {
        return ERR_EMFILE;
    }

    Kernel::Process::copy_fd_slot(proc->fd_table[new_idx], proc->fd_table[old_idx], true);
    return new_idx;
}

int64_t sys_dup3(uint64_t raw_oldfd, uint64_t raw_newfd, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || raw_oldfd >= Kernel::Process::MAX_FDS_PER_PROCESS ||
        raw_newfd >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }
    if (raw_oldfd == raw_newfd) {
        return ERR_EINVAL;
    }
    if ((flags & ~O_CLOEXEC) != 0ULL) {
        return ERR_EINVAL;
    }

    const auto old_idx = static_cast<size_t>(raw_oldfd);
    const auto new_idx = static_cast<size_t>(raw_newfd);
    if (!proc->fd_table[old_idx].in_use) {
        return ERR_EBADF;
    }

    if (proc->fd_table[new_idx].in_use) {
        Kernel::Process::close_fd(*proc, new_idx);
    }

    Kernel::Process::copy_fd_slot(proc->fd_table[new_idx], proc->fd_table[old_idx], true);
    if ((flags & O_CLOEXEC) != 0ULL) {
        proc->fd_table[new_idx].flags |= Kernel::Process::FD_FLAG_CLOEXEC;
    }
    return static_cast<int64_t>(new_idx);
}

int64_t sys_fcntl(uint64_t raw_fd, uint64_t cmd, uint64_t arg) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || raw_fd >= Kernel::Process::MAX_FDS_PER_PROCESS) {
        return ERR_EBADF;
    }
    const auto fd_idx = static_cast<size_t>(raw_fd);
    auto& desc = proc->fd_table[fd_idx];
    if (!desc.in_use) {
        return ERR_EBADF;
    }

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if (arg >= Kernel::Process::MAX_FDS_PER_PROCESS) {
            return ERR_EINVAL;
        }
        const int new_idx = allocate_fd_slot(*proc, static_cast<size_t>(arg));
        if (new_idx < 0) {
            return ERR_EMFILE;
        }
        Kernel::Process::copy_fd_slot(proc->fd_table[new_idx], desc, true);
        if (cmd == F_DUPFD_CLOEXEC) {
            proc->fd_table[new_idx].flags |= Kernel::Process::FD_FLAG_CLOEXEC;
        }
        return new_idx;
    }

    case F_GETFD:
        return ((desc.flags & Kernel::Process::FD_FLAG_CLOEXEC) != 0U)
                   ? static_cast<int64_t>(FD_CLOEXEC_BIT)
                   : 0;

    case F_SETFD:
        if ((arg & FD_CLOEXEC_BIT) != 0ULL) {
            desc.flags |= Kernel::Process::FD_FLAG_CLOEXEC;
        } else {
            desc.flags &= ~Kernel::Process::FD_FLAG_CLOEXEC;
        }
        return 0;

    case F_GETFL: {
        uint64_t flags = O_RDONLY;
        if (desc.is_readable() && desc.is_writable()) {
            flags = O_RDWR;
        } else if (desc.is_writable()) {
            flags = O_WRONLY;
        }
        if ((desc.flags & Kernel::Process::FD_FLAG_NONBLOCK) != 0U) {
            flags |= O_NONBLOCK;
        }
        return static_cast<int64_t>(flags);
    }

    case F_SETFL:
        if ((arg & O_NONBLOCK) != 0ULL) {
            desc.flags |= Kernel::Process::FD_FLAG_NONBLOCK;
        } else {
            desc.flags &= ~Kernel::Process::FD_FLAG_NONBLOCK;
        }
        return 0;

    default:
        return ERR_EINVAL;
    }
}

int64_t sys_pipe2(uintptr_t user_pipefd, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(O_CLOEXEC | O_NONBLOCK)) != 0ULL) {
        return ERR_EINVAL;
    }

    const int read_fd = allocate_fd_slot(*proc, 0U);
    if (read_fd < 0) {
        return ERR_EMFILE;
    }
    proc->fd_table[read_fd].in_use = true;

    const int write_fd = allocate_fd_slot(*proc, static_cast<size_t>(read_fd) + 1U);
    if (write_fd < 0) {
        proc->fd_table[read_fd].in_use = false;
        return ERR_EMFILE;
    }

    Kernel::FS::Pipe* const pipe = Kernel::FS::allocate_pipe();
    if (pipe == nullptr) {
        proc->fd_table[read_fd].in_use = false;
        return ERR_EMFILE;
    }

    uint32_t extra_flags = 0U;
    if ((flags & O_CLOEXEC) != 0ULL) {
        extra_flags |= Kernel::Process::FD_FLAG_CLOEXEC;
    }
    if ((flags & O_NONBLOCK) != 0ULL) {
        extra_flags |= Kernel::Process::FD_FLAG_NONBLOCK;
    }

    proc->fd_table[read_fd] = Kernel::Process::FileDescriptor{
        .type = Kernel::Process::FdType::PipeRead,
        .flags = Kernel::Process::FD_FLAG_READ | extra_flags,
        .offset = 0U,
        .in_use = true,
        .pipe = pipe,
    };

    proc->fd_table[write_fd] = Kernel::Process::FileDescriptor{
        .type = Kernel::Process::FdType::PipeWrite,
        .flags = Kernel::Process::FD_FLAG_WRITE | extra_flags,
        .offset = 0U,
        .in_use = true,
        .pipe = pipe,
    };

    const int32_t pair[2] = {read_fd, write_fd};
    if (Kernel::MM::copy_to_user(proc->address_space, user_pipefd, pair, sizeof(pair)) < 0) {
        Kernel::Process::close_fd(*proc, static_cast<size_t>(read_fd));
        Kernel::Process::close_fd(*proc, static_cast<size_t>(write_fd));
        return ERR_EFAULT;
    }

    return 0;
}

// ==========================================================================
// TTY & I/O Syscalls
// ==========================================================================

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

    case IOCTL_TIOCGPGRP: {
        const auto pgrp = static_cast<int32_t>(g_foreground_pgid);
        return Kernel::MM::copy_to_user(root_paddr, arg, &pgrp, sizeof(pgrp));
    }

    case IOCTL_TIOCSPGRP: {
        int32_t new_pgrp = 0;
        const int64_t copy_res =
            Kernel::MM::copy_from_user(root_paddr, &new_pgrp, arg, sizeof(new_pgrp));
        if (copy_res < 0) {
            return copy_res;
        }
        if (new_pgrp <= 0) {
            return ERR_EINVAL;
        }
        g_foreground_pgid = static_cast<uint32_t>(new_pgrp);
        return 0;
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

    if (raw_count > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }
    const auto count = static_cast<size_t>(raw_count);

    if (proc != nullptr) {
        auto& desc = proc->fd_table[fd_idx];
        if (!desc.in_use || !desc.is_readable()) {
            return ERR_EBADF;
        }
        root_paddr = proc->address_space.root_paddr();

        if (!validate_user_write_range(root_paddr, user_buf, count)) {
            return ERR_EFAULT;
        }
        if (count == 0U) {
            return 0;
        }

        if (desc.type == Kernel::Process::FdType::InitrdFile) {
            if (desc.cpio_entry.is_directory) {
                return ERR_EISDIR;
            }
            if (desc.offset >= desc.cpio_entry.size || desc.cpio_entry.data == nullptr) {
                return 0;
            }
            const size_t avail = desc.cpio_entry.size - desc.offset;
            const size_t to_copy = (count < avail) ? count : avail;
            const int64_t res = Kernel::MM::copy_to_user(
                root_paddr, user_buf, desc.cpio_entry.data + desc.offset, to_copy);
            if (res < 0) {
                return ERR_EFAULT;
            }
            desc.offset += to_copy;
            return static_cast<int64_t>(to_copy);
        }

        if (desc.type == Kernel::Process::FdType::PipeRead) {
            const bool nonblock = (desc.flags & Kernel::Process::FD_FLAG_NONBLOCK) != 0U;
            return Kernel::FS::pipe_read(desc.pipe, root_paddr, user_buf, count, nonblock);
        }

        if (desc.type != Kernel::Process::FdType::ConsoleTty) {
            return ERR_EBADF;
        }
    } else {
        if (fd_idx != 0U) {
            return ERR_EBADF;
        }
        root_paddr = Kernel::MM::AddressSpace::current_root_paddr();
        if (!validate_user_write_range(root_paddr, user_buf, count)) {
            return ERR_EFAULT;
        }
        if (count == 0U) {
            return 0;
        }
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

    if (raw_count > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }
    const auto count = static_cast<size_t>(raw_count);

    if (proc != nullptr) {
        const auto& desc = proc->fd_table[fd_idx];
        if (!desc.in_use || !desc.is_writable()) {
            return ERR_EBADF;
        }
        root_paddr = proc->address_space.root_paddr();

        if (!validate_user_read_range(root_paddr, user_buf, count)) {
            return ERR_EFAULT;
        }
        if (count == 0U) {
            return 0;
        }

        if (desc.type == Kernel::Process::FdType::PipeWrite) {
            const bool nonblock = (desc.flags & Kernel::Process::FD_FLAG_NONBLOCK) != 0U;
            return Kernel::FS::pipe_write(desc.pipe, root_paddr, user_buf, count, nonblock);
        }

        if (desc.type != Kernel::Process::FdType::ConsoleTty) {
            return ERR_EBADF;
        }
    } else {
        if (fd_idx != 1U && fd_idx != 2U) {
            return ERR_EBADF;
        }
        root_paddr = Kernel::MM::AddressSpace::current_root_paddr();
        if (!validate_user_read_range(root_paddr, user_buf, count)) {
            return ERR_EFAULT;
        }
        if (count == 0U) {
            return 0;
        }
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

    uint64_t total_written = 0ULL;
    for (size_t idx = 0U; idx < iovcnt; ++idx) {
        UserIovec iov{};
        const uintptr_t entry_vaddr = user_iov + (idx * sizeof(UserIovec));
        if (Kernel::MM::copy_from_user(root_paddr, &iov, entry_vaddr, sizeof(UserIovec)) < 0) {
            return (total_written > 0ULL) ? static_cast<int64_t>(total_written) : ERR_EFAULT;
        }
        if (iov.iov_len == 0ULL) {
            continue;
        }
        const int64_t wres = sys_write(raw_fd, static_cast<uintptr_t>(iov.iov_base), iov.iov_len);
        if (wres < 0) {
            return (total_written > 0ULL) ? static_cast<int64_t>(total_written) : wres;
        }
        total_written += static_cast<uint64_t>(wres);
    }

    return static_cast<int64_t>(total_written);
}

// ==========================================================================
// Signals & Process Group Job Control
// ==========================================================================

int64_t sys_rt_sigaction(uint64_t sig, uintptr_t user_act, uintptr_t user_oldact,
                         uint64_t sigsetsize) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (sigsetsize != sizeof(uint64_t) || sig == 0U || sig > Kernel::Process::MAX_SIGNALS) {
        return ERR_EINVAL;
    }

    const size_t idx = static_cast<size_t>(sig - 1U);
    if (user_oldact != 0U) {
        const auto& cur = proc->sigactions[idx];
        const LinuxSigAction out{
            .sa_handler = cur.handler,
            .sa_flags = cur.flags,
            .sa_restorer = cur.restorer,
            .sa_mask = cur.mask,
        };
        if (Kernel::MM::copy_to_user(proc->address_space, user_oldact, &out, sizeof(out)) < 0) {
            return ERR_EFAULT;
        }
    }

    if (user_act != 0U) {
        if (sig == Kernel::Process::SIG_KILL || sig == Kernel::Process::SIG_STOP) {
            return ERR_EINVAL;
        }
        LinuxSigAction dest{};
        if (Kernel::MM::copy_from_user(proc->address_space, &dest, user_act, sizeof(dest)) < 0) {
            return ERR_EFAULT;
        }
        proc->sigactions[idx] = Kernel::Process::SigAction{
            .handler = dest.sa_handler,
            .flags = dest.sa_flags,
            .restorer = dest.sa_restorer,
            .mask = dest.sa_mask,
        };
    }

    return 0;
}

int64_t sys_rt_sigprocmask(uint64_t how, uintptr_t user_set, uintptr_t user_oldset,
                           uint64_t sigsetsize) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || sigsetsize != sizeof(uint64_t)) {
        return ERR_EINVAL;
    }

    if (user_oldset != 0U) {
        const uint64_t old_mask = proc->blocked_signals;
        if (Kernel::MM::copy_to_user(proc->address_space, user_oldset, &old_mask,
                                     sizeof(old_mask)) < 0) {
            return ERR_EFAULT;
        }
    }

    if (user_set != 0U) {
        uint64_t set_val = 0ULL;
        if (Kernel::MM::copy_from_user(proc->address_space, &set_val, user_set, sizeof(set_val)) <
            0) {
            return ERR_EFAULT;
        }
        constexpr uint64_t UNMASKABLE =
            (1ULL << (Kernel::Process::SIG_KILL - 1U)) | (1ULL << (Kernel::Process::SIG_STOP - 1U));
        set_val &= ~UNMASKABLE;

        if (how == SIG_BLOCK) {
            proc->blocked_signals |= set_val;
        } else if (how == SIG_UNBLOCK) {
            proc->blocked_signals &= ~set_val;
        } else if (how == SIG_SETMASK) {
            proc->blocked_signals = set_val;
        } else {
            return ERR_EINVAL;
        }
    }

    return 0;
}

int64_t sys_kill(int32_t pid, uint64_t sig) noexcept {
    auto* const caller = Kernel::Process::current_process();
    if (caller == nullptr) {
        return ERR_EINVAL;
    }
    if (sig > Kernel::Process::MAX_SIGNALS) {
        return ERR_EINVAL;
    }

    bool delivered_any = false;
    if (pid > 0) {
        auto* const target = Kernel::Process::find_process_by_pid(static_cast<uint32_t>(pid));
        if (target == nullptr || target->state == Kernel::Process::ProcessState::Zombie) {
            return ERR_ESRCH;
        }
        if (sig > 0U) {
            Kernel::Process::send_signal(*target, static_cast<uint32_t>(sig));
        }
        return 0;
    }

    const uint32_t target_pgid = (pid == 0) ? caller->pgid : static_cast<uint32_t>(-pid);
    for (size_t i = 0U; i < Kernel::Process::MAX_PROCESSES; ++i) {
        auto* const proc = Kernel::Process::process_at(i);
        if (proc == nullptr || proc->state == Kernel::Process::ProcessState::Unused ||
            proc->state == Kernel::Process::ProcessState::Zombie) {
            continue;
        }
        if (pid == -1) {
            if (proc->pid > 1U && proc != caller) {
                if (sig > 0U) {
                    Kernel::Process::send_signal(*proc, static_cast<uint32_t>(sig));
                }
                delivered_any = true;
            }
        } else if (proc->pgid == target_pgid) {
            if (sig > 0U) {
                Kernel::Process::send_signal(*proc, static_cast<uint32_t>(sig));
            }
            delivered_any = true;
        }
    }

    return delivered_any ? 0 : ERR_ESRCH;
}

int64_t sys_setpgid(int32_t raw_pid, int32_t raw_pgid) noexcept {
    auto* const caller = Kernel::Process::current_process();
    if (caller == nullptr || raw_pid < 0 || raw_pgid < 0) {
        return ERR_EINVAL;
    }

    const uint32_t target_pid = (raw_pid == 0) ? caller->pid : static_cast<uint32_t>(raw_pid);
    auto* const target = Kernel::Process::find_process_by_pid(target_pid);
    if (target == nullptr) {
        return ERR_ESRCH;
    }
    if (target != caller && target->ppid != caller->pid) {
        return ERR_ESRCH;
    }

    const uint32_t new_pgid = (raw_pgid == 0) ? target->pid : static_cast<uint32_t>(raw_pgid);
    target->pgid = new_pgid;
    return 0;
}

int64_t sys_getpgid(int32_t raw_pid) noexcept {
    auto* const caller = Kernel::Process::current_process();
    if (caller == nullptr || raw_pid < 0) {
        return ERR_EINVAL;
    }
    if (raw_pid == 0) {
        return static_cast<int64_t>(caller->pgid);
    }
    auto* const target = Kernel::Process::find_process_by_pid(static_cast<uint32_t>(raw_pid));
    if (target == nullptr) {
        return ERR_ESRCH;
    }
    return static_cast<int64_t>(target->pgid);
}

int64_t sys_getsid(int32_t raw_pid) noexcept {
    auto* const caller = Kernel::Process::current_process();
    if (caller == nullptr || raw_pid < 0) {
        return ERR_EINVAL;
    }
    if (raw_pid == 0) {
        return static_cast<int64_t>(caller->sid);
    }
    auto* const target = Kernel::Process::find_process_by_pid(static_cast<uint32_t>(raw_pid));
    if (target == nullptr) {
        return ERR_ESRCH;
    }
    return static_cast<int64_t>(target->sid);
}

int64_t sys_setsid() noexcept {
    auto* const caller = Kernel::Process::current_process();
    if (caller == nullptr) {
        return ERR_EINVAL;
    }
    if (caller->pgid == caller->pid) {
        return ERR_EPERM;
    }
    caller->sid = caller->pid;
    caller->pgid = caller->pid;
    return static_cast<int64_t>(caller->sid);
}

// ==========================================================================
// Process Lifecycle, Memory Management & System Identity
// ==========================================================================

[[noreturn]] void sys_exit(uint64_t raw_status) noexcept {
    const auto exit_code =
        static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(raw_status)));
    if (Kernel::Process::current_process() != nullptr) {
        Kernel::Process::exit_current_process(exit_code);
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

    const auto platform = HAL::get_platform_info();
    const size_t page_size = platform.page_size;
    const uintptr_t page_mask = page_size - 1U;
    const uintptr_t va_limit = Kernel::MM::user_addr_space_limit();
    const uintptr_t stack_floor =
        platform.user_stack_top - Kernel::Process::DEFAULT_USER_STACK_SIZE;

    if (length == 0U || (length & page_mask) != 0U) {
        return ERR_EINVAL;
    }
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0U) {
        return ERR_EINVAL;
    }
    if ((flags & MAP_ANONYMOUS) == 0U || (flags & MAP_PRIVATE) == 0U) {
        return ERR_EINVAL;
    }
    if ((flags & MAP_SHARED) != 0U) {
        return ERR_EINVAL;
    }
    if (static_cast<int64_t>(raw_fd) != -1 || offset != 0U) {
        return ERR_EINVAL;
    }

    const bool is_fixed = (flags & MAP_FIXED) != 0U;
    uintptr_t map_start = 0U;
    uintptr_t map_end = 0U;
    bool used_arena_bump = false;

    if (is_fixed) {
        if (addr == 0U || (addr & page_mask) != 0U || addr >= va_limit ||
            length > (va_limit - addr)) {
            return ERR_EINVAL;
        }
        map_start = addr;
        map_end = addr + static_cast<uintptr_t>(length);
        if (map_end > stack_floor) {
            return ERR_ENOMEM;
        }
        // MAP_FIXED replaces existing mappings in [map_start, map_end)
        unmap_and_free_range(proc->address_space, map_start, map_end, page_size);
    } else if (addr != 0U && (addr & page_mask) == 0U && addr < stack_floor &&
               length <= (stack_floor - addr) && addr >= MMAP_ARENA_FLOOR) {
        // Check if requested hint range is completely unmapped
        bool hint_free = true;
        for (uintptr_t page = addr; page < addr + static_cast<uintptr_t>(length);
             page += page_size) {
            uintptr_t tmp_pa = 0U;
            HAL::PageFlags tmp_fl = HAL::PageFlags::None;
            if (proc->address_space.query_user_page(page, tmp_pa, tmp_fl)) {
                hint_free = false;
                break;
            }
        }
        if (hint_free) {
            map_start = addr;
            map_end = addr + static_cast<uintptr_t>(length);
        }
    }

    if (map_start == 0U) {
        const uintptr_t arena_top = proc->mmap_next_vaddr;
        if (length > arena_top || (arena_top - static_cast<uintptr_t>(length)) < MMAP_ARENA_FLOOR) {
            return ERR_ENOMEM;
        }
        map_start = arena_top - static_cast<uintptr_t>(length);
        map_end = arena_top;
        if (map_start <= proc->brk_current) {
            return ERR_ENOMEM;
        }
        used_arena_bump = true;
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

    for (uintptr_t page_vaddr = map_start; page_vaddr < map_end; page_vaddr += page_size) {
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

    if (used_arena_bump) {
        proc->mmap_next_vaddr = map_start;
    }
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
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->entry_point = parent->entry_point;
    child->brk_start = parent->brk_start;
    child->brk_current = parent->brk_current;
    child->mmap_next_vaddr = parent->mmap_next_vaddr;
    child->user_stack = Kernel::Process::UserStack{};
    child->blocked_signals = parent->blocked_signals;
    cstr_copy(child->cwd, parent->cwd, sizeof(child->cwd));

    for (size_t i = 0U; i < Kernel::Process::MAX_SIGNALS; ++i) {
        child->sigactions[i] = parent->sigactions[i];
    }

    for (size_t idx = 0U; idx < Kernel::Process::MAX_FDS_PER_PROCESS; ++idx) {
        Kernel::Process::copy_fd_slot(child->fd_table[idx], parent->fd_table[idx], false);
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
    child->kctx_initialized = false;
    Kernel::Scheduler::enqueue(child);

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
    char* const raw_path_buf = scratch_kva;
    char* const string_pool = scratch_kva + EXEC_MAX_PATH_LEN;
    const size_t string_pool_cap = page_size - EXEC_MAX_PATH_LEN;

    const char* argv_kptrs[EXEC_MAX_ARGS + 1U]{};
    const char* envp_kptrs[EXEC_MAX_ENVS + 1U]{};

    const uintptr_t caller_root = proc->address_space.root_paddr();
    size_t path_len = 0U;
    const int64_t path_res =
        copy_user_cstring(caller_root, user_pathname, raw_path_buf, EXEC_MAX_PATH_LEN, path_len);
    if (path_res < 0) {
        free_frame(scratch_paddr);
        return path_res;
    }

    char canon_path[Kernel::Process::MAX_PATH_LEN]{};
    if (!normalize_vfs_path(proc->cwd, raw_path_buf, canon_path, sizeof(canon_path))) {
        free_frame(scratch_paddr);
        return ERR_ENOENT;
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

    CpioEntry entry{};
    if (!resolve_initrd_path(canon_path, entry)) {
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

    loaded.address_space.activate();

    if (proc->user_stack.phys_base != 0U && proc->user_stack.num_pages > 0U) {
        free_frames(proc->user_stack.phys_base, proc->user_stack.num_pages);
    }
    proc->address_space = static_cast<Kernel::MM::AddressSpace&&>(loaded.address_space);

    // Close FD_CLOEXEC files and reset custom signal handlers on execve
    Kernel::Process::close_cloexec_fds(*proc);
    for (size_t i = 0U; i < Kernel::Process::MAX_SIGNALS; ++i) {
        if (proc->sigactions[i].handler != Kernel::Process::KERNEL_SIG_IGN) {
            proc->sigactions[i] = Kernel::Process::SigAction{};
        }
    }

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

    if (target_pid < -1) {
        // Wait for any child in process group (-target_pid)
    }

    Kernel::Process::Process* target_child = nullptr;

    while (true) {
        HAL::get_cpu().disable_interrupts();

        bool has_matching_child = false;
        Kernel::Process::Process* zombie_child = nullptr;

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
            if (target_pid == 0 && candidate->pgid != parent->pgid) {
                continue;
            }
            if (target_pid < -1 && candidate->pgid != static_cast<uint32_t>(-target_pid)) {
                continue;
            }

            has_matching_child = true;
            if (candidate->state == Kernel::Process::ProcessState::Zombie) {
                zombie_child = candidate;
                break;
            }
        }

        if (!has_matching_child) {
            return ERR_ECHILD;
        }

        if (zombie_child != nullptr) {
            target_child = zombie_child;
            break;
        }

        if ((options & WAIT_WNOHANG) != 0ULL) {
            return 0;
        }

        parent->child_wait_queue.sleep();
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

int64_t sys_uname(uintptr_t user_buf) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    LinuxUtsname uts{};
    cstr_copy(uts.sysname, "Linux", sizeof(uts.sysname));
    cstr_copy(uts.nodename, "milo", sizeof(uts.nodename));
    cstr_copy(uts.release, "6.6.0-milo", sizeof(uts.release));
    cstr_copy(uts.version, "#1 SMP PREEMPT", sizeof(uts.version));
    cstr_copy(uts.machine, "aarch64", sizeof(uts.machine));
    cstr_copy(uts.domainname, "(none)", sizeof(uts.domainname));

    return Kernel::MM::copy_to_user(proc->address_space, user_buf, &uts, sizeof(uts));
}

int64_t sys_clock_gettime(uint64_t clk_id, uintptr_t user_tp) noexcept {
    (void)clk_id;
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
    const uint64_t ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
    LinuxTimespec time_spec{};
    if (freq > 0ULL) {
        time_spec.tv_sec = static_cast<int64_t>(ticks / freq);
        const uint64_t rem = ticks % freq;
        time_spec.tv_nsec = static_cast<int64_t>((rem * 1000000000ULL) / freq);
    }

    return Kernel::MM::copy_to_user(proc->address_space, user_tp, &time_spec, sizeof(time_spec));
}

int64_t sys_sysinfo(uintptr_t user_info) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
    const uint64_t ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
    const auto platform = HAL::get_platform_info();

    LinuxSysinfo info{};
    info.uptime = (freq > 0ULL) ? static_cast<int64_t>(ticks / freq) : 0;
    info.totalram = platform.ram_size;
    info.freeram = pfa_free_pages_count() * platform.page_size;
    info.procs = static_cast<uint16_t>(Kernel::Process::active_process_count());
    info.mem_unit = 1U;

    return Kernel::MM::copy_to_user(proc->address_space, user_info, &info, sizeof(info));
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
        case SYS_GETCWD:
            ret_val = sys_getcwd(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                 cpu.get_syscall_arg(ctx, 1U));
            break;

        case SYS_DUP:
            ret_val = sys_dup(cpu.get_syscall_arg(ctx, 0U));
            break;

        case SYS_DUP3:
            ret_val = sys_dup3(cpu.get_syscall_arg(ctx, 0U), cpu.get_syscall_arg(ctx, 1U),
                               cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_FCNTL:
            ret_val = sys_fcntl(cpu.get_syscall_arg(ctx, 0U), cpu.get_syscall_arg(ctx, 1U),
                                cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_IOCTL:
            ret_val = sys_ioctl(cpu.get_syscall_arg(ctx, 0U), cpu.get_syscall_arg(ctx, 1U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)));
            break;

        case SYS_FACCESSAT:
            ret_val = sys_faccessat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_CHDIR:
            ret_val = sys_chdir(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_OPENAT:
            ret_val = sys_openat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_CLOSE:
            ret_val = sys_close(cpu.get_syscall_arg(ctx, 0U));
            break;

        case SYS_PIPE2:
            ret_val = sys_pipe2(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                cpu.get_syscall_arg(ctx, 1U));
            break;

        case SYS_GETDENTS64:
            ret_val = sys_getdents64(cpu.get_syscall_arg(ctx, 0U),
                                     static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                     cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_LSEEK:
            ret_val = sys_lseek(cpu.get_syscall_arg(ctx, 0U),
                                static_cast<int64_t>(cpu.get_syscall_arg(ctx, 1U)),
                                cpu.get_syscall_arg(ctx, 2U));
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

        case SYS_WRITEV:
            ret_val = sys_writev(cpu.get_syscall_arg(ctx, 0U),
                                 static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                 cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_READLINKAT:
            ret_val = sys_readlinkat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)), cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_NEWFSTATAT:
            ret_val = sys_newfstatat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)), cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_FSTAT:
            ret_val = sys_fstat(cpu.get_syscall_arg(ctx, 0U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            sys_exit(cpu.get_syscall_arg(ctx, 0U));

        case SYS_SET_TID_ADDRESS:
            ret_val = sys_set_tid_address(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_CLOCK_GETTIME:
            ret_val = sys_clock_gettime(cpu.get_syscall_arg(ctx, 0U),
                                        static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_SCHED_YIELD:
            Kernel::Scheduler::yield();
            ret_val = 0;
            break;

        case SYS_KILL:
            ret_val =
                sys_kill(static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                         cpu.get_syscall_arg(ctx, 1U));
            break;

        case SYS_RT_SIGACTION:
            ret_val = sys_rt_sigaction(
                cpu.get_syscall_arg(ctx, 0U), static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)), cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_RT_SIGPROCMASK:
            ret_val = sys_rt_sigprocmask(
                cpu.get_syscall_arg(ctx, 0U), static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)), cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_RT_SIGRETURN:
            ret_val = Kernel::Process::restore_signal_frame(ctx);
            break;

        case SYS_SETPGID:
            ret_val = sys_setpgid(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 1U))));
            break;

        case SYS_GETPGID:
            ret_val = sys_getpgid(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))));
            break;

        case SYS_GETSID:
            ret_val = sys_getsid(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))));
            break;

        case SYS_SETSID:
            ret_val = sys_setsid();
            break;

        case SYS_UNAME:
            ret_val = sys_uname(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_GETPID:
            ret_val = sys_getpid();
            break;

        case SYS_GETPPID:
            ret_val = sys_getppid();
            break;

        case SYS_GETUID:
        case SYS_GETEUID:
        case SYS_GETGID:
        case SYS_GETEGID:
            ret_val = 0;
            break;

        case SYS_SYSINFO:
            ret_val = sys_sysinfo(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
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

        case SYS_MPROTECT:
            ret_val = sys_mprotect(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                   cpu.get_syscall_arg(ctx, 1U), cpu.get_syscall_arg(ctx, 2U));
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
                return;
            }
            break;

        case SYS_WAIT4:
            ret_val = sys_wait4(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)));
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