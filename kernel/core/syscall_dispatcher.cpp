#include "fs/devfs/devfs.hpp"
#include "fs/vfs.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "hal/trap.hpp"
#include "ipc/pipe.hpp"
#include "loader/elf_loader.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"
#include "process/process.hpp"
#include "process/resource.hpp"
#include "process/scheduler.hpp"
#include "process/stack_builder.hpp"

#include <cstddef>
#include <cstdint>
#include <new>

namespace {

// Linux AArch64 Syscall Numbers
constexpr uint64_t SYS_GETCWD = 17U;
constexpr uint64_t SYS_DUP = 23U;
constexpr uint64_t SYS_DUP3 = 24U;
constexpr uint64_t SYS_FCNTL = 25U;
constexpr uint64_t SYS_IOCTL = 29U;
constexpr uint64_t SYS_MKDIRAT = 34U;
constexpr uint64_t SYS_UNLINKAT = 35U;
constexpr uint64_t SYS_RENAMEAT = 38U;
constexpr uint64_t SYS_TRUNCATE = 45U;
constexpr uint64_t SYS_FTRUNCATE = 46U;
constexpr uint64_t SYS_FACCESSAT = 48U;
constexpr uint64_t SYS_CHDIR = 49U;
constexpr uint64_t SYS_FCHMOD = 52U;
constexpr uint64_t SYS_FCHMODAT = 53U;
constexpr uint64_t SYS_FCHOWNAT = 54U;
constexpr uint64_t SYS_FCHOWN = 55U;
constexpr uint64_t SYS_OPENAT = 56U;
constexpr uint64_t SYS_CLOSE = 57U;
constexpr uint64_t SYS_PIPE2 = 59U;
constexpr uint64_t SYS_GETDENTS64 = 61U;
constexpr uint64_t SYS_LSEEK = 62U;
constexpr uint64_t SYS_READ = 63U;
constexpr uint64_t SYS_WRITE = 64U;
constexpr uint64_t SYS_WRITEV = 66U;
constexpr uint64_t SYS_PSELECT6 = 72U;
constexpr uint64_t SYS_PPOLL = 73U;
constexpr uint64_t SYS_READLINKAT = 78U;
constexpr uint64_t SYS_NEWFSTATAT = 79U;
constexpr uint64_t SYS_FSTAT = 80U;
constexpr uint64_t SYS_FSYNC = 82U;
constexpr uint64_t SYS_FDATASYNC = 83U;
constexpr uint64_t SYS_UTIMENSAT = 88U;
constexpr uint64_t SYS_EXIT = 93U;
constexpr uint64_t SYS_EXIT_GROUP = 94U;
constexpr uint64_t SYS_SET_TID_ADDRESS = 96U;
constexpr uint64_t SYS_NANOSLEEP = 101U;
constexpr uint64_t SYS_CLOCK_GETTIME = 113U;
constexpr uint64_t SYS_SCHED_YIELD = 124U;
constexpr uint64_t SYS_KILL = 129U;
constexpr uint64_t SYS_RT_SIGACTION = 134U;
constexpr uint64_t SYS_RT_SIGPROCMASK = 135U;
constexpr uint64_t SYS_RT_SIGRETURN = 139U;
constexpr uint64_t SYS_TIMES = 153U;
constexpr uint64_t SYS_SETPGID = 154U;
constexpr uint64_t SYS_GETPGID = 155U;
constexpr uint64_t SYS_GETSID = 156U;
constexpr uint64_t SYS_SETSID = 157U;
constexpr uint64_t SYS_UNAME = 160U;
constexpr uint64_t SYS_GETRUSAGE = 165U;
constexpr uint64_t SYS_UMASK = 166U;
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
constexpr uint64_t SYS_PRLIMIT64 = 261U;
constexpr uint64_t SYS_RENAMEAT2 = 276U;
constexpr uint64_t SYS_GETRANDOM = 278U;
constexpr uint64_t SYS_FACCESSAT2 = 439U;

constexpr size_t UIO_MAXIOV = 1024U;

// Negated POSIX Error Codes
constexpr int64_t ERR_EPERM = -1;
constexpr int64_t ERR_ENOENT = -2;
constexpr int64_t ERR_ESRCH = -3;
constexpr int64_t ERR_EINTR = -4;
constexpr int64_t ERR_E2BIG = -7;
constexpr int64_t ERR_ENOEXEC = -8;
constexpr int64_t ERR_EBADF = -9;
constexpr int64_t ERR_ECHILD = -10;
constexpr int64_t ERR_EAGAIN = -11;
constexpr int64_t ERR_ENOMEM = -12;
constexpr int64_t ERR_EACCES = -13;
constexpr int64_t ERR_EFAULT = -14;
constexpr int64_t ERR_EEXIST = -17;
constexpr int64_t ERR_ENOTDIR = -20;
constexpr int64_t ERR_EINVAL = -22;
constexpr int64_t ERR_EROFS = -30;
constexpr int64_t ERR_ERANGE = -34;
constexpr int64_t ERR_ENOSYS = -38;

// File / Open / Fcntl Constants
constexpr int32_t AT_FDCWD = -100;
constexpr uint64_t AT_SYMLINK_NOFOLLOW = 0x100ULL;
constexpr uint64_t AT_EACCESS = 0x200ULL;
constexpr uint64_t AT_REMOVEDIR = 0x200ULL;
constexpr uint64_t AT_EMPTY_PATH = 0x1000ULL;

constexpr uint64_t O_ACCMODE = 0x3ULL;
constexpr uint64_t O_RDONLY = 0x0ULL;
constexpr uint64_t O_WRONLY = 0x1ULL;
constexpr uint64_t O_RDWR = 0x2ULL;
constexpr uint64_t O_CREAT = 0x40ULL;
constexpr uint64_t O_EXCL = 0x80ULL;
constexpr uint64_t O_TRUNC = 0x200ULL;
constexpr uint64_t O_APPEND = 0x400ULL;
constexpr uint64_t O_DIRECTORY = 0x4000ULL; // AArch64 O_DIRECTORY = 040000
constexpr uint64_t O_NONBLOCK = 0x800ULL;
constexpr uint64_t O_CLOEXEC = 0x80000ULL;

constexpr uint64_t RENAME_NOREPLACE = 1ULL;
constexpr int64_t UTIME_NOW = 0x3FFFFFFFLL;
constexpr int64_t UTIME_OMIT = 0x3FFFFFFELL;

constexpr uint64_t F_DUPFD = 0ULL;
constexpr uint64_t F_GETFD = 1ULL;
constexpr uint64_t F_SETFD = 2ULL;
constexpr uint64_t F_GETFL = 3ULL;
constexpr uint64_t F_SETFL = 4ULL;
constexpr uint64_t F_DUPFD_CLOEXEC = 1030ULL;
constexpr uint64_t FD_CLOEXEC_BIT = 1ULL;

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
constexpr size_t EXEC_MAX_ARGS = 64U;
constexpr size_t EXEC_MAX_ENVS = 64U;

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

constexpr size_t READ_CHUNK_SIZE = 256U;
constexpr size_t WRITE_CHUNK_SIZE = 256U;

// Signal masks
constexpr uint64_t SIG_BLOCK = 0ULL;
constexpr uint64_t SIG_UNBLOCK = 1ULL;
constexpr uint64_t SIG_SETMASK = 2ULL;

// getrandom Flags
constexpr uint64_t GRND_NONBLOCK = 0x0001ULL;
constexpr uint64_t GRND_RANDOM = 0x0002ULL;
constexpr uint64_t GRND_INSECURE = 0x0004ULL;

// getrusage Selectors
constexpr int32_t RUSAGE_SELF = 0;
constexpr int32_t RUSAGE_CHILDREN = -1;
constexpr int32_t RUSAGE_THREAD = 1;

// poll Event Bitmasks
constexpr int16_t POLLIN = 0x0001;
constexpr int16_t POLLOUT = 0x0004;
constexpr int16_t POLLERR = 0x0008;
constexpr int16_t POLLHUP = 0x0010;
constexpr int16_t POLLNVAL = 0x0020;
constexpr int16_t POLLRDNORM = 0x0040;
constexpr int16_t POLLWRNORM = 0x0100;

constexpr size_t MAX_FDSET_FDS = 1024U;
constexpr size_t MAX_FDSET_WORDS = MAX_FDSET_FDS / 64U;

constexpr size_t POLL_STACK_THRESHOLD = 32U;

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

struct LinuxTms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};
static_assert(sizeof(LinuxTms) == 32U, "LinuxTms must match Linux ABI (32 bytes)");

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

struct LinuxTimeval {
    int64_t tv_sec;
    int64_t tv_usec;
};
static_assert(sizeof(LinuxTimeval) == 16U, "LinuxTimeval must be 16 bytes");

struct LinuxRusage {
    LinuxTimeval ru_utime;
    LinuxTimeval ru_stime;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
};
static_assert(sizeof(LinuxRusage) == LINUX_RUSAGE_SIZE,
              "LinuxRusage must match Linux AArch64 ABI (144 bytes)");

struct LinuxRlimit64 {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};
static_assert(sizeof(LinuxRlimit64) == 16U, "LinuxRlimit64 must be 16 bytes");

struct LinuxPollFd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};
static_assert(sizeof(LinuxPollFd) == 8U, "LinuxPollFd must be 8 bytes");

struct LinuxSigsetArgpack {
    uint64_t ss;
    uint64_t ss_len;
};
static_assert(sizeof(LinuxSigsetArgpack) == 16U, "LinuxSigsetArgpack must be 16 bytes");

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
};
static_assert(sizeof(LinuxSysinfo) == 112U, "LinuxSysinfo must be 112 bytes");

void wait_for_event_or_tick() noexcept {
    auto& cpu = HAL::get_cpu();
    Kernel::Scheduler::yield();
    cpu.enable_interrupts();
    cpu.halt();
    cpu.disable_interrupts();
}

uint64_t timespec_to_timer_ticks(const LinuxTimespec& time_spec, uint64_t freq) noexcept {
    if (freq == 0ULL || (time_spec.tv_sec == 0 && time_spec.tv_nsec == 0)) {
        return 0ULL;
    }
    const uint64_t sec_ticks = static_cast<uint64_t>(time_spec.tv_sec) * freq;
    const uint64_t nsec_ticks =
        (static_cast<uint64_t>(time_spec.tv_nsec) * freq + 999999999ULL) / 1000000000ULL;
    return sec_ticks + nsec_ticks;
}

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

kernel::fs::File* get_proc_file(Kernel::Process::Process* proc, uint64_t raw_fd) noexcept {
    if (proc == nullptr || proc->fd_table == nullptr || raw_fd >= proc->fd_capacity) {
        return nullptr;
    }
    auto& desc = proc->fd_table[static_cast<size_t>(raw_fd)];
    return desc.is_open() ? desc.file : nullptr;
}

void fill_stat_from_inode(const kernel::fs::Inode& inode, LinuxStat& stat_out) noexcept {
    stat_out = LinuxStat{};
    stat_out.st_dev = 1ULL;
    stat_out.st_ino = (inode.ino != 0U) ? inode.ino : 1ULL;
    stat_out.st_mode = inode.mode;
    stat_out.st_nlink = (inode.nlink != 0U) ? inode.nlink : 1U;
    stat_out.st_uid = inode.uid;
    stat_out.st_gid = inode.gid;
    stat_out.st_rdev = inode.rdev;
    stat_out.st_size = static_cast<int64_t>(inode.size);
    stat_out.st_blksize = 4096;
    stat_out.st_blocks = static_cast<int64_t>((inode.size + 511U) / 512U);
    stat_out.st_atime_sec = static_cast<int64_t>(inode.atime_ns / 1000000000ULL);
    stat_out.st_atime_nsec = static_cast<int64_t>(inode.atime_ns % 1000000000ULL);
    stat_out.st_mtime_sec = static_cast<int64_t>(inode.mtime_ns / 1000000000ULL);
    stat_out.st_mtime_nsec = static_cast<int64_t>(inode.mtime_ns % 1000000000ULL);
    stat_out.st_ctime_sec = static_cast<int64_t>(inode.ctime_ns / 1000000000ULL);
    stat_out.st_ctime_nsec = static_cast<int64_t>(inode.ctime_ns % 1000000000ULL);
}

void unmap_and_free_range(Kernel::MM::AddressSpace& adrsp, uintptr_t start_vaddr,
                          uintptr_t end_vaddr, size_t page_size) noexcept {
    constexpr uint64_t PTE_DEMAND_ZERO = 1ULL << 56U;
    const uintptr_t page_mask = page_size - 1U;

    for (uintptr_t page_vaddr = start_vaddr; page_vaddr < end_vaddr; page_vaddr += page_size) {
        uintptr_t out_paddr = 0U;
        HAL::PageFlags out_flags = HAL::PageFlags::None;
        if (adrsp.query_user_page(page_vaddr, out_paddr, out_flags)) {
            adrsp.unmap_user_page(page_vaddr);
            free_frame(out_paddr & ~page_mask);
        } else {
            uint64_t raw_pte = 0ULL;
            if (adrsp.query_pte_flags(page_vaddr, raw_pte) && (raw_pte & PTE_DEMAND_ZERO) != 0ULL) {
                adrsp.unmap_user_page(page_vaddr);
            }
        }
    }
}

bool validate_user_range(uintptr_t root_paddr, uintptr_t user_buf, size_t count,
                         HAL::PageFlags required_perm) noexcept {
    constexpr uint64_t DESC_COW = 1ULL << 55U;
    constexpr uint64_t PTE_DEMAND_ZERO = 1ULL << 56U;
    constexpr uint64_t PTE_DZ_FLAGS_SHIFT = 8U;
    constexpr uint64_t PTE_DZ_FLAGS_MASK = 0xFFULL << PTE_DZ_FLAGS_SHIFT;

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
            uint64_t raw_pte = 0ULL;
            if (!Kernel::MM::AddressSpace::query_pte_flags(root_paddr, page_vaddr, raw_pte) ||
                (raw_pte & PTE_DEMAND_ZERO) == 0ULL) {
                return false;
            }
            flags =
                static_cast<HAL::PageFlags>((raw_pte & PTE_DZ_FLAGS_MASK) >> PTE_DZ_FLAGS_SHIFT);
        } else if (!HAL::has_flag(flags, HAL::PageFlags::Write)) {
            uint64_t raw_pte = 0ULL;
            if (Kernel::MM::AddressSpace::query_pte_flags(root_paddr, page_vaddr, raw_pte) &&
                (raw_pte & DESC_COW) != 0ULL) {
                flags |= HAL::PageFlags::Write;
            }
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

int64_t resolve_dirfd_dentry(Kernel::Process::Process& proc, int32_t dirfd,
                             kernel::fs::Dentry*& out_base) noexcept {
    if (dirfd == AT_FDCWD) {
        return kernel::fs::vfs_namei(kernel::fs::StringView(proc.cwd), nullptr, &out_base);
    }
    kernel::fs::File* dir_file = get_proc_file(&proc, static_cast<uint64_t>(dirfd));
    if (dir_file == nullptr) {
        return ERR_EBADF;
    }
    if (dir_file->inode == nullptr || !dir_file->inode->is_dir() || dir_file->dentry == nullptr) {
        return ERR_ENOTDIR;
    }
    kernel::fs::dentry_get(dir_file->dentry);
    out_base = dir_file->dentry;
    return 0;
}

void build_dentry_path(const kernel::fs::Dentry* target, char* out, size_t cap) noexcept {
    if (out == nullptr || cap < 2U) {
        return;
    }
    if (target == nullptr || target->is_root() || target->parent == nullptr) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    const kernel::fs::Dentry* chain[32]{};
    size_t depth = 0U;
    for (const kernel::fs::Dentry* cur = target;
         cur != nullptr && cur->parent != nullptr && depth < 32U; cur = cur->parent) {
        chain[depth++] = cur;
    }
    size_t pos = 0U;
    for (size_t idx = depth; idx > 0U; --idx) {
        const auto name = chain[idx - 1U]->name_view();
        if (pos + 1U + name.size() >= cap) {
            break;
        }
        out[pos++] = '/';
        for (size_t cidx = 0U; cidx < name.size(); ++cidx) {
            out[pos++] = name[cidx];
        }
    }
    if (pos == 0U) {
        out[pos++] = '/';
    }
    out[pos] = '\0';
}

// ==========================================================================
// VFS, Directory, Pipe & Descriptor Syscalls
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

    kernel::fs::Dentry* cwd_dentry = nullptr;
    if (raw_path[0] != '/') {
        (void)kernel::fs::vfs_namei(kernel::fs::StringView(proc->cwd), nullptr, &cwd_dentry);
    }

    kernel::fs::Dentry* target = nullptr;
    const int32_t vfs_res =
        kernel::fs::vfs_namei(kernel::fs::StringView(raw_path, len), cwd_dentry, &target);
    if (cwd_dentry != nullptr) {
        kernel::fs::dentry_put(cwd_dentry);
    }
    if (vfs_res != kernel::fs::kOk) {
        return vfs_res;
    }
    if (target->inode == nullptr || !target->inode->is_dir()) {
        kernel::fs::dentry_put(target);
        return ERR_ENOTDIR;
    }

    build_dentry_path(target, proc->cwd, sizeof(proc->cwd));
    kernel::fs::dentry_put(target);
    return 0;
}

int64_t sys_openat(int32_t dirfd, uintptr_t user_pathname, uint64_t flags, uint64_t mode) noexcept {
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

    kernel::fs::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const uint64_t accmode = flags & O_ACCMODE;
    uint32_t vfs_flags = 0U;
    if (accmode == O_RDONLY) {
        vfs_flags = kernel::fs::kOpenReadOnly;
    } else if (accmode == O_WRONLY) {
        vfs_flags = kernel::fs::kOpenWriteOnly;
    } else if (accmode == O_RDWR) {
        vfs_flags = kernel::fs::kOpenReadWrite;
    } else {
        if (base_dentry != nullptr) {
            kernel::fs::dentry_put(base_dentry);
        }
        return ERR_EINVAL;
    }

    if ((flags & O_CREAT) != 0ULL) {
        vfs_flags |= kernel::fs::kOpenCreate;
    }
    if ((flags & O_EXCL) != 0ULL) {
        vfs_flags |= kernel::fs::kOpenExcl;
    }
    if ((flags & O_TRUNC) != 0ULL) {
        vfs_flags |= kernel::fs::kOpenTrunc;
    }
    if ((flags & O_APPEND) != 0ULL) {
        vfs_flags |= kernel::fs::kOpenAppend;
    }
    if ((flags & O_DIRECTORY) != 0ULL) {
        vfs_flags |= kernel::fs::kOpenDirectory;
    }
    if ((flags & O_NONBLOCK) != 0ULL) {
        vfs_flags |= kernel::fs::kOpenNonBlock;
    }

    const int slot = Kernel::Process::allocate_fd(*proc);
    if (slot < 0) {
        if (base_dentry != nullptr) {
            kernel::fs::dentry_put(base_dentry);
        }
        return slot;
    }

    const uint32_t effective_mode = static_cast<uint32_t>(mode) & ~proc->umask;
    kernel::fs::File* file = nullptr;
    const int32_t open_rc = kernel::fs::vfs_open_at(
        base_dentry, kernel::fs::StringView(raw_path, path_len), vfs_flags, effective_mode, &file);
    if (base_dentry != nullptr) {
        kernel::fs::dentry_put(base_dentry);
    }
    if (open_rc != kernel::fs::kOk) {
        return open_rc;
    }

    const uint32_t fd_flags = ((flags & O_CLOEXEC) != 0ULL) ? Kernel::Process::FD_FLAG_CLOEXEC : 0U;
    proc->fd_table[slot] = Kernel::Process::FileDescriptor{
        .file = file,
        .fd_flags = fd_flags,
    };
    return slot;
}

uint64_t monotonic_now_ns() noexcept {
    const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
    const uint64_t ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
    if (freq == 0ULL) {
        return 0ULL;
    }
    const uint64_t secs = ticks / freq;
    const uint64_t rem = ticks % freq;
    return (secs * 1000000000ULL) + ((rem * 1000000000ULL) / freq);
}

int64_t sys_mkdirat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode) noexcept {
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

    kernel::fs::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const uint32_t effective_mode = static_cast<uint32_t>(mode) & ~proc->umask;
    const int32_t vfs_res = kernel::fs::vfs_mkdir_at(
        base_dentry, kernel::fs::StringView(raw_path, path_len), effective_mode);
    if (base_dentry != nullptr) {
        kernel::fs::dentry_put(base_dentry);
    }
    return vfs_res;
}

int64_t sys_unlinkat(int32_t dirfd, uintptr_t user_pathname, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~AT_REMOVEDIR) != 0ULL) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space.root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    kernel::fs::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const kernel::fs::StringView path_view(raw_path, path_len);
    const int32_t vfs_res = ((flags & AT_REMOVEDIR) != 0ULL)
                                ? kernel::fs::vfs_rmdir_at(base_dentry, path_view)
                                : kernel::fs::vfs_unlink_at(base_dentry, path_view);
    if (base_dentry != nullptr) {
        kernel::fs::dentry_put(base_dentry);
    }
    return vfs_res;
}

int64_t sys_renameat2(int32_t olddirfd, uintptr_t user_oldpath, int32_t newdirfd,
                      uintptr_t user_newpath, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~RENAME_NOREPLACE) != 0ULL) {
        return ERR_EINVAL;
    }

    char old_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t old_len = 0U;
    const int64_t old_copy = copy_user_cstring(proc->address_space.root_paddr(), user_oldpath,
                                               old_path, sizeof(old_path), old_len);
    if (old_copy < 0) {
        return old_copy;
    }

    char new_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t new_len = 0U;
    const int64_t new_copy = copy_user_cstring(proc->address_space.root_paddr(), user_newpath,
                                               new_path, sizeof(new_path), new_len);
    if (new_copy < 0) {
        return new_copy;
    }

    kernel::fs::Dentry* old_base = nullptr;
    if (old_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, olddirfd, old_base);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    kernel::fs::Dentry* new_base = nullptr;
    if (new_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, newdirfd, new_base);
        if (dir_res < 0) {
            if (old_base != nullptr) {
                kernel::fs::dentry_put(old_base);
            }
            return dir_res;
        }
    }

    if ((flags & RENAME_NOREPLACE) != 0ULL) {
        kernel::fs::Dentry* existing = nullptr;
        if (kernel::fs::vfs_namei(kernel::fs::StringView(new_path, new_len), new_base, &existing,
                                  false) == kernel::fs::kOk) {
            kernel::fs::dentry_put(existing);
            if (new_base != nullptr) {
                kernel::fs::dentry_put(new_base);
            }
            if (old_base != nullptr) {
                kernel::fs::dentry_put(old_base);
            }
            return ERR_EEXIST;
        }
    }

    const int32_t vfs_res =
        kernel::fs::vfs_rename_at(old_base, kernel::fs::StringView(old_path, old_len), new_base,
                                  kernel::fs::StringView(new_path, new_len));
    if (new_base != nullptr) {
        kernel::fs::dentry_put(new_base);
    }
    if (old_base != nullptr) {
        kernel::fs::dentry_put(old_base);
    }
    return vfs_res;
}

int64_t sys_renameat(int32_t olddirfd, uintptr_t user_oldpath, int32_t newdirfd,
                     uintptr_t user_newpath) noexcept {
    return sys_renameat2(olddirfd, user_oldpath, newdirfd, user_newpath, 0ULL);
}

int64_t sys_ftruncate(uint64_t raw_fd, int64_t length) noexcept {
    if (length < 0) {
        return ERR_EINVAL;
    }
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || proc->fd_table == nullptr || raw_fd >= proc->fd_capacity) {
        return ERR_EBADF;
    }
    const auto& desc = proc->fd_table[static_cast<size_t>(raw_fd)];
    if (!desc.is_open() || !desc.is_writable() || desc.file->inode == nullptr) {
        return ERR_EBADF;
    }
    kernel::fs::Inode* inode = desc.file->inode;
    if (inode->sb != nullptr && inode->sb->is_read_only()) {
        return ERR_EROFS;
    }
    if (inode->i_ops == nullptr || inode->i_ops->truncate == nullptr) {
        return ERR_EINVAL;
    }
    return inode->i_ops->truncate(inode, static_cast<uint64_t>(length));
}

int64_t sys_truncate(uintptr_t user_pathname, int64_t length) noexcept {
    if (length < 0) {
        return ERR_EINVAL;
    }
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

    kernel::fs::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, AT_FDCWD, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    kernel::fs::Dentry* target = nullptr;
    const int32_t vfs_res = kernel::fs::vfs_namei(kernel::fs::StringView(raw_path, path_len),
                                                  base_dentry, &target, true);
    if (base_dentry != nullptr) {
        kernel::fs::dentry_put(base_dentry);
    }
    if (vfs_res != kernel::fs::kOk || target == nullptr || target->inode == nullptr) {
        if (target != nullptr) {
            kernel::fs::dentry_put(target);
        }
        return (vfs_res != kernel::fs::kOk) ? vfs_res : ERR_ENOENT;
    }

    kernel::fs::Inode* inode = target->inode;
    int64_t res = 0;
    if (inode->sb != nullptr && inode->sb->is_read_only()) {
        res = ERR_EROFS;
    } else if (inode->i_ops == nullptr || inode->i_ops->truncate == nullptr) {
        res = ERR_EINVAL;
    } else {
        res = inode->i_ops->truncate(inode, static_cast<uint64_t>(length));
    }
    kernel::fs::dentry_put(target);
    return res;
}

int64_t sys_utimensat(int32_t dirfd, uintptr_t user_pathname, uintptr_t user_times,
                      uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0ULL) {
        return ERR_EINVAL;
    }

    LinuxTimespec times[2]{
        {.tv_sec = 0, .tv_nsec = UTIME_NOW},
        {.tv_sec = 0, .tv_nsec = UTIME_NOW},
    };
    if (user_times != 0U) {
        if (Kernel::MM::copy_from_user(proc->address_space, times, user_times, sizeof(times)) < 0) {
            return ERR_EFAULT;
        }
        for (const auto& time_val : times) {
            if (time_val.tv_nsec != UTIME_NOW && time_val.tv_nsec != UTIME_OMIT &&
                (time_val.tv_nsec < 0 || time_val.tv_nsec >= 1000000000LL)) {
                return ERR_EINVAL;
            }
        }
    }

    kernel::fs::Inode* inode = nullptr;
    kernel::fs::Dentry* target = nullptr;

    if (user_pathname == 0U) {
        kernel::fs::File* file = get_proc_file(proc, static_cast<uint64_t>(dirfd));
        if (file == nullptr || file->inode == nullptr) {
            return ERR_EBADF;
        }
        inode = file->inode;
    } else {
        char raw_path[Kernel::Process::MAX_PATH_LEN]{};
        size_t path_len = 0U;
        const int64_t copy_res = copy_user_cstring(proc->address_space.root_paddr(), user_pathname,
                                                   raw_path, sizeof(raw_path), path_len);
        if (copy_res < 0) {
            return copy_res;
        }

        if (path_len == 0U && (flags & AT_EMPTY_PATH) != 0ULL) {
            kernel::fs::File* file = get_proc_file(proc, static_cast<uint64_t>(dirfd));
            if (file == nullptr || file->inode == nullptr) {
                return ERR_EBADF;
            }
            inode = file->inode;
        } else {
            kernel::fs::Dentry* base_dentry = nullptr;
            if (raw_path[0] != '/') {
                const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
                if (dir_res < 0) {
                    return dir_res;
                }
            }
            const bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0ULL;
            const int32_t vfs_res = kernel::fs::vfs_namei(
                kernel::fs::StringView(raw_path, path_len), base_dentry, &target, follow);
            if (base_dentry != nullptr) {
                kernel::fs::dentry_put(base_dentry);
            }
            if (vfs_res != kernel::fs::kOk || target == nullptr || target->inode == nullptr) {
                if (target != nullptr) {
                    kernel::fs::dentry_put(target);
                }
                return (vfs_res != kernel::fs::kOk) ? vfs_res : ERR_ENOENT;
            }
            inode = target->inode;
        }
    }

    int64_t res = 0;
    if (inode->sb != nullptr && inode->sb->is_read_only()) {
        res = ERR_EROFS;
    } else {
        const uint64_t now_ns = monotonic_now_ns();
        if (times[0].tv_nsec == UTIME_NOW) {
            inode->atime_ns = now_ns;
        } else if (times[0].tv_nsec != UTIME_OMIT) {
            inode->atime_ns = (static_cast<uint64_t>(times[0].tv_sec) * 1000000000ULL) +
                              static_cast<uint64_t>(times[0].tv_nsec);
        }
        if (times[1].tv_nsec == UTIME_NOW) {
            inode->mtime_ns = now_ns;
        } else if (times[1].tv_nsec != UTIME_OMIT) {
            inode->mtime_ns = (static_cast<uint64_t>(times[1].tv_sec) * 1000000000ULL) +
                              static_cast<uint64_t>(times[1].tv_nsec);
        }
        inode->ctime_ns = now_ns;
    }

    if (target != nullptr) {
        kernel::fs::dentry_put(target);
    }
    return res;
}

int64_t sys_fsync(uint64_t raw_fd, int32_t datasync) noexcept {
    auto* const proc = Kernel::Process::current_process();
    kernel::fs::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr) {
        return ERR_EBADF;
    }
    if (file->f_ops != nullptr && file->f_ops->fsync != nullptr) {
        return file->f_ops->fsync(file, datasync);
    }
    return 0;
}

int64_t sys_fchmod(uint64_t raw_fd, uint64_t mode) noexcept {
    auto* const proc = Kernel::Process::current_process();
    kernel::fs::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr || file->inode == nullptr) {
        return ERR_EBADF;
    }
    if (file->inode->sb != nullptr && file->inode->sb->is_read_only()) {
        return ERR_EROFS;
    }
    file->inode->mode =
        (file->inode->mode & kernel::fs::kModeTypeMask) | (static_cast<uint32_t>(mode) & 07777U);
    file->inode->ctime_ns = monotonic_now_ns();
    return 0;
}

int64_t sys_fchmodat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                     uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0ULL) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space.root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    kernel::fs::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0ULL;
    kernel::fs::Dentry* target = nullptr;
    const int32_t vfs_res = kernel::fs::vfs_namei(kernel::fs::StringView(raw_path, path_len),
                                                  base_dentry, &target, follow);
    if (base_dentry != nullptr) {
        kernel::fs::dentry_put(base_dentry);
    }
    if (vfs_res != kernel::fs::kOk || target == nullptr || target->inode == nullptr) {
        if (target != nullptr) {
            kernel::fs::dentry_put(target);
        }
        return (vfs_res != kernel::fs::kOk) ? vfs_res : ERR_ENOENT;
    }

    int64_t res = 0;
    if (target->inode->sb != nullptr && target->inode->sb->is_read_only()) {
        res = ERR_EROFS;
    } else {
        target->inode->mode = (target->inode->mode & kernel::fs::kModeTypeMask) |
                              (static_cast<uint32_t>(mode) & 07777U);
        target->inode->ctime_ns = monotonic_now_ns();
    }
    kernel::fs::dentry_put(target);
    return res;
}

int64_t sys_fchown(uint64_t raw_fd, uint32_t owner, uint32_t group) noexcept {
    auto* const proc = Kernel::Process::current_process();
    kernel::fs::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr || file->inode == nullptr) {
        return ERR_EBADF;
    }
    if (file->inode->sb != nullptr && file->inode->sb->is_read_only()) {
        return ERR_EROFS;
    }
    if (owner != static_cast<uint32_t>(-1)) {
        file->inode->uid = owner;
    }
    if (group != static_cast<uint32_t>(-1)) {
        file->inode->gid = group;
    }
    file->inode->ctime_ns = monotonic_now_ns();
    return 0;
}

int64_t sys_fchownat(int32_t dirfd, uintptr_t user_pathname, uint32_t owner, uint32_t group,
                     uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0ULL) {
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
        return sys_fchown(static_cast<uint64_t>(dirfd), owner, group);
    }

    kernel::fs::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0ULL;
    kernel::fs::Dentry* target = nullptr;
    const int32_t vfs_res = kernel::fs::vfs_namei(kernel::fs::StringView(raw_path, path_len),
                                                  base_dentry, &target, follow);
    if (base_dentry != nullptr) {
        kernel::fs::dentry_put(base_dentry);
    }
    if (vfs_res != kernel::fs::kOk || target == nullptr || target->inode == nullptr) {
        if (target != nullptr) {
            kernel::fs::dentry_put(target);
        }
        return (vfs_res != kernel::fs::kOk) ? vfs_res : ERR_ENOENT;
    }

    int64_t res = 0;
    if (target->inode->sb != nullptr && target->inode->sb->is_read_only()) {
        res = ERR_EROFS;
    } else {
        if (owner != static_cast<uint32_t>(-1)) {
            target->inode->uid = owner;
        }
        if (group != static_cast<uint32_t>(-1)) {
            target->inode->gid = group;
        }
        target->inode->ctime_ns = monotonic_now_ns();
    }
    kernel::fs::dentry_put(target);
    return res;
}

int64_t sys_close(uint64_t raw_fd) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || proc->fd_table == nullptr || raw_fd >= proc->fd_capacity) {
        return ERR_EBADF;
    }
    const auto fd_idx = static_cast<size_t>(raw_fd);
    if (!proc->fd_table[fd_idx].is_open()) {
        return ERR_EBADF;
    }
    Kernel::Process::close_fd(*proc, fd_idx);
    return 0;
}

int64_t sys_lseek(uint64_t raw_fd, int64_t offset, uint64_t whence) noexcept {
    auto* const proc = Kernel::Process::current_process();
    kernel::fs::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr) {
        return ERR_EBADF;
    }
    if (whence > SEEK_END) {
        return ERR_EINVAL;
    }
    return kernel::fs::vfs_lseek(file, offset,
                                 static_cast<kernel::fs::SeekWhence>(static_cast<int32_t>(whence)));
}

int64_t sys_fstat(uint64_t raw_fd, uintptr_t user_statbuf) noexcept {
    auto* const proc = Kernel::Process::current_process();
    kernel::fs::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr || file->inode == nullptr) {
        return ERR_EBADF;
    }
    LinuxStat stat{};
    fill_stat_from_inode(*file->inode, stat);
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
        kernel::fs::File* file = get_proc_file(proc, static_cast<uint64_t>(dirfd));
        if (file == nullptr || file->inode == nullptr) {
            return ERR_EBADF;
        }
        LinuxStat stat{};
        fill_stat_from_inode(*file->inode, stat);
        return Kernel::MM::copy_to_user(proc->address_space, user_statbuf, &stat, sizeof(stat));
    }

    kernel::fs::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0ULL;
    kernel::fs::Dentry* target = nullptr;
    const int32_t vfs_res = kernel::fs::vfs_namei(kernel::fs::StringView(raw_path, path_len),
                                                  base_dentry, &target, follow);
    if (base_dentry != nullptr) {
        kernel::fs::dentry_put(base_dentry);
    }
    if (vfs_res != kernel::fs::kOk || target->inode == nullptr) {
        if (target != nullptr) {
            kernel::fs::dentry_put(target);
        }
        return (vfs_res != kernel::fs::kOk) ? vfs_res : ERR_ENOENT;
    }

    LinuxStat stat{};
    fill_stat_from_inode(*target->inode, stat);
    kernel::fs::dentry_put(target);
    return Kernel::MM::copy_to_user(proc->address_space, user_statbuf, &stat, sizeof(stat));
}

int64_t sys_faccessat2(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                       uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EACCESS | AT_EMPTY_PATH)) != 0ULL ||
        (mode & ~7ULL) != 0ULL) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space.root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    kernel::fs::Inode* inode = nullptr;
    kernel::fs::Dentry* target = nullptr;

    if (path_len == 0U && (flags & AT_EMPTY_PATH) != 0ULL) {
        kernel::fs::File* file = get_proc_file(proc, static_cast<uint64_t>(dirfd));
        if (file == nullptr || file->inode == nullptr) {
            return ERR_EBADF;
        }
        inode = file->inode;
    } else {
        kernel::fs::Dentry* base_dentry = nullptr;
        if (raw_path[0] != '/') {
            const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
            if (dir_res < 0) {
                return dir_res;
            }
        }
        const bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0ULL;
        const int32_t vfs_res = kernel::fs::vfs_namei(kernel::fs::StringView(raw_path, path_len),
                                                      base_dentry, &target, follow);
        if (base_dentry != nullptr) {
            kernel::fs::dentry_put(base_dentry);
        }
        if (vfs_res != kernel::fs::kOk || target->inode == nullptr) {
            if (target != nullptr) {
                kernel::fs::dentry_put(target);
            }
            return (vfs_res != kernel::fs::kOk) ? vfs_res : ERR_ENOENT;
        }
        inode = target->inode;
    }

    int64_t res = 0;
    if ((mode & 2ULL) != 0ULL && inode->sb != nullptr && inode->sb->is_read_only()) {
        res = ERR_EROFS;
    } else if ((mode & 1ULL) != 0ULL && !inode->is_dir() && (inode->mode & 0111U) == 0U) {
        res = ERR_EACCES;
    }

    if (target != nullptr) {
        kernel::fs::dentry_put(target);
    }
    return res;
}

int64_t sys_faccessat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                      uint64_t flags) noexcept {
    return sys_faccessat2(dirfd, user_pathname, mode, flags);
}

int64_t sys_readlinkat(int32_t dirfd, uintptr_t user_pathname, uintptr_t user_buf,
                       uint64_t bufsiz) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || bufsiz == 0U) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space.root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    if (cstr_eq(raw_path, "/proc/self/exe")) {
        const char* exe_target = (proc->exe_path[0] != '\0') ? proc->exe_path : "/bin/bash";
        const size_t target_len = cstr_len(exe_target);
        const size_t copy_len = (bufsiz < target_len) ? static_cast<size_t>(bufsiz) : target_len;
        if (Kernel::MM::copy_to_user(proc->address_space, user_buf, exe_target, copy_len) < 0) {
            return ERR_EFAULT;
        }
        return static_cast<int64_t>(copy_len);
    }

    kernel::fs::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    char link_buf[Kernel::Process::MAX_PATH_LEN]{};
    size_t out_len = 0U;
    const int32_t vfs_res =
        kernel::fs::vfs_readlink(kernel::fs::StringView(raw_path, path_len), base_dentry, link_buf,
                                 sizeof(link_buf), &out_len);
    if (base_dentry != nullptr) {
        kernel::fs::dentry_put(base_dentry);
    }
    if (vfs_res != kernel::fs::kOk) {
        return vfs_res;
    }

    const size_t copy_len = (bufsiz < out_len) ? static_cast<size_t>(bufsiz) : out_len;
    if (copy_len > 0U &&
        Kernel::MM::copy_to_user(proc->address_space, user_buf, link_buf, copy_len) < 0) {
        return ERR_EFAULT;
    }
    return static_cast<int64_t>(copy_len);
}

int64_t sys_getdents64(uint64_t raw_fd, uintptr_t user_dirp, uint64_t count) noexcept {
    auto* const proc = Kernel::Process::current_process();
    kernel::fs::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr || file->inode == nullptr) {
        return ERR_EBADF;
    }
    if (!file->inode->is_dir()) {
        return ERR_ENOTDIR;
    }

    const uintptr_t root_paddr = proc->address_space.root_paddr();
    if (!validate_user_write_range(root_paddr, user_dirp, static_cast<size_t>(count))) {
        return ERR_EFAULT;
    }

    size_t bytes_written = 0U;
    auto push_dirent = [&](uint64_t ino, uint8_t d_type, const char* name, size_t name_len,
                           uint64_t next_off) -> bool {
        const size_t raw_reclen = sizeof(LinuxDirent64Header) + name_len + 1U;
        const size_t aligned_reclen = (raw_reclen + 7U) & ~7U;
        if (bytes_written + aligned_reclen > count || aligned_reclen > 128U) {
            return false;
        }
        uint8_t record[128]{};
        auto* const hdr = reinterpret_cast<LinuxDirent64Header*>(record);
        hdr->d_ino = ino;
        hdr->d_off = static_cast<int64_t>(next_off);
        hdr->d_reclen = static_cast<uint16_t>(aligned_reclen);
        hdr->d_type = d_type;
        for (size_t idx = 0U; idx < name_len; ++idx) {
            record[sizeof(LinuxDirent64Header) + idx] = static_cast<uint8_t>(name[idx]);
        }
        if (Kernel::MM::copy_to_user(root_paddr, user_dirp + bytes_written, record,
                                     aligned_reclen) < 0) {
            return false;
        }
        bytes_written += aligned_reclen;
        return true;
    };

    if (file->offset == 0U) {
        if (!push_dirent(file->inode->ino, DT_DIR, ".", 1U, 1U)) {
            return static_cast<int64_t>(bytes_written);
        }
        file->offset = 1U;
    }

    if (file->offset == 1U) {
        const uint64_t parent_ino = (file->dentry != nullptr && file->dentry->parent != nullptr &&
                                     file->dentry->parent->inode != nullptr)
                                        ? file->dentry->parent->inode->ino
                                        : file->inode->ino;
        if (!push_dirent(parent_ino, DT_DIR, "..", 2U, 2U)) {
            return static_cast<int64_t>(bytes_written);
        }
        file->offset = 2U;
    }

    while (bytes_written < count) {
        const uint64_t child_idx = file->offset - 2U;
        file->offset = child_idx;
        kernel::fs::DirEntryInfo info{};
        const int32_t vfs_res = kernel::fs::vfs_readdir(file, &info);
        file->offset = child_idx + 2U;
        if (vfs_res != kernel::fs::kOk) {
            break;
        }

        uint8_t dtype = DT_REG;
        if (info.type == kernel::fs::kModeDir) {
            dtype = DT_DIR;
        } else if (info.type == kernel::fs::kModeLnk) {
            dtype = DT_LNK;
        }

        const size_t name_len = cstr_len(info.name);
        if (!push_dirent(info.ino, dtype, info.name, name_len, child_idx + 3U)) {
            break;
        }
        file->offset = child_idx + 3U;
    }

    return static_cast<int64_t>(bytes_written);
}

int64_t sys_dup(uint64_t raw_oldfd) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || proc->fd_table == nullptr || raw_oldfd >= proc->fd_capacity) {
        return ERR_EBADF;
    }
    const auto old_idx = static_cast<size_t>(raw_oldfd);
    if (!proc->fd_table[old_idx].is_open()) {
        return ERR_EBADF;
    }

    const int new_idx = Kernel::Process::allocate_fd(*proc, 0U);
    if (new_idx < 0) {
        return new_idx;
    }

    Kernel::Process::copy_fd_slot(proc->fd_table[new_idx], proc->fd_table[old_idx], true);
    return new_idx;
}

int64_t sys_dup3(uint64_t raw_oldfd, uint64_t raw_newfd, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || proc->fd_table == nullptr || raw_oldfd >= proc->fd_capacity) {
        return ERR_EBADF;
    }
    if (raw_newfd >= proc->rlim_nofile_cur) {
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
    if (!proc->fd_table[old_idx].is_open()) {
        return ERR_EBADF;
    }

    if (new_idx >= proc->fd_capacity) {
        if (!Kernel::Process::ensure_fd_capacity(*proc, new_idx + 1U)) {
            return ERR_ENOMEM;
        }
    }

    if (proc->fd_table[new_idx].is_open()) {
        Kernel::Process::close_fd(*proc, new_idx);
    }

    Kernel::Process::copy_fd_slot(proc->fd_table[new_idx], proc->fd_table[old_idx], true);
    if ((flags & O_CLOEXEC) != 0ULL) {
        proc->fd_table[new_idx].fd_flags |= Kernel::Process::FD_FLAG_CLOEXEC;
    }
    return static_cast<int64_t>(new_idx);
}

int64_t sys_fcntl(uint64_t raw_fd, uint64_t cmd, uint64_t arg) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || proc->fd_table == nullptr || raw_fd >= proc->fd_capacity) {
        return ERR_EBADF;
    }
    const auto fd_idx = static_cast<size_t>(raw_fd);
    if (!proc->fd_table[fd_idx].is_open()) {
        return ERR_EBADF;
    }

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if (arg >= proc->rlim_nofile_cur) {
            return ERR_EINVAL;
        }
        const int new_idx = Kernel::Process::allocate_fd(*proc, static_cast<size_t>(arg));
        if (new_idx < 0) {
            return new_idx;
        }
        Kernel::Process::copy_fd_slot(proc->fd_table[new_idx], proc->fd_table[fd_idx], true);
        if (cmd == F_DUPFD_CLOEXEC) {
            proc->fd_table[new_idx].fd_flags |= Kernel::Process::FD_FLAG_CLOEXEC;
        }
        return new_idx;
    }

    case F_GETFD:
        return ((proc->fd_table[fd_idx].fd_flags & Kernel::Process::FD_FLAG_CLOEXEC) != 0U)
                   ? static_cast<int64_t>(FD_CLOEXEC_BIT)
                   : 0;

    case F_SETFD:
        if ((arg & FD_CLOEXEC_BIT) != 0ULL) {
            proc->fd_table[fd_idx].fd_flags |= Kernel::Process::FD_FLAG_CLOEXEC;
        } else {
            proc->fd_table[fd_idx].fd_flags &= ~Kernel::Process::FD_FLAG_CLOEXEC;
        }
        return 0;

    case F_GETFL: {
        const auto& desc = proc->fd_table[fd_idx];
        uint64_t flags = O_RDONLY;
        if (desc.is_readable() && desc.is_writable()) {
            flags = O_RDWR;
        } else if (desc.is_writable()) {
            flags = O_WRONLY;
        }
        if ((desc.file->flags & kernel::fs::kOpenNonBlock) != 0U) {
            flags |= O_NONBLOCK;
        }
        if ((desc.file->flags & kernel::fs::kOpenAppend) != 0U) {
            flags |= O_APPEND;
        }
        return static_cast<int64_t>(flags);
    }

    case F_SETFL:
        if ((arg & O_NONBLOCK) != 0ULL) {
            proc->fd_table[fd_idx].file->flags |= kernel::fs::kOpenNonBlock;
        } else {
            proc->fd_table[fd_idx].file->flags &= ~kernel::fs::kOpenNonBlock;
        }
        if ((arg & O_APPEND) != 0ULL) {
            proc->fd_table[fd_idx].file->flags |= kernel::fs::kOpenAppend;
        } else {
            proc->fd_table[fd_idx].file->flags &= ~kernel::fs::kOpenAppend;
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

    const int read_fd = Kernel::Process::allocate_fd(*proc, 0U);
    if (read_fd < 0) {
        return read_fd;
    }

    const uint32_t open_extra = ((flags & O_NONBLOCK) != 0ULL) ? kernel::fs::kOpenNonBlock : 0U;
    const uint32_t fd_extra = ((flags & O_CLOEXEC) != 0ULL) ? Kernel::Process::FD_FLAG_CLOEXEC : 0U;

    kernel::fs::File* read_file = nullptr;
    kernel::fs::File* write_file = nullptr;
    const int32_t prc = Kernel::IPC::create_pipe_files(&read_file, &write_file, open_extra);
    if (prc != kernel::fs::kOk) {
        return prc;
    }

    proc->fd_table[read_fd] = Kernel::Process::FileDescriptor{
        .file = read_file,
        .fd_flags = fd_extra,
    };

    const int write_fd = Kernel::Process::allocate_fd(*proc, static_cast<size_t>(read_fd) + 1U);
    if (write_fd < 0) {
        Kernel::Process::close_fd(*proc, static_cast<size_t>(read_fd));
        (void)kernel::fs::vfs_close(write_file);
        return write_fd;
    }

    proc->fd_table[write_fd] = Kernel::Process::FileDescriptor{
        .file = write_file,
        .fd_flags = fd_extra,
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
// TTY, Multiplexing & I/O Syscalls
// ==========================================================================

int64_t sys_ioctl(uint64_t raw_fd, uint64_t request, uintptr_t arg) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc != nullptr) {
        kernel::fs::File* file = get_proc_file(proc, raw_fd);
        if (file == nullptr) {
            return ERR_EBADF;
        }
        return kernel::fs::vfs_ioctl(file, request, static_cast<uint64_t>(arg));
    }

    if (raw_fd > 2U) {
        return ERR_EBADF;
    }
    kernel::fs::File* console_file = nullptr;
    const int32_t orc = kernel::fs::devfs_open_console(kernel::fs::kOpenReadWrite, &console_file);
    if (orc != kernel::fs::kOk) {
        return orc;
    }
    const int32_t res = kernel::fs::vfs_ioctl(console_file, request, static_cast<uint64_t>(arg));
    (void)kernel::fs::vfs_close(console_file);
    return res;
}

int64_t sys_read(uint64_t raw_fd, uintptr_t user_buf, uint64_t raw_count) noexcept {
    if (raw_count > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }
    const auto count = static_cast<size_t>(raw_count);
    auto* const proc = Kernel::Process::current_process();

    kernel::fs::File* file = nullptr;
    bool temp_console = false;
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        file = get_proc_file(proc, raw_fd);
        if (file == nullptr) {
            return ERR_EBADF;
        }
        root_paddr = proc->address_space.root_paddr();
    } else {
        if (raw_fd != 0U) {
            return ERR_EBADF;
        }
        root_paddr = Kernel::MM::AddressSpace::current_root_paddr();
        if (kernel::fs::devfs_open_console(kernel::fs::kOpenReadOnly, &file) != kernel::fs::kOk) {
            return ERR_EBADF;
        }
        temp_console = true;
    }

    if (!validate_user_write_range(root_paddr, user_buf, count)) {
        if (temp_console) {
            (void)kernel::fs::vfs_close(file);
        }
        return ERR_EFAULT;
    }
    if (count == 0U) {
        const int64_t zero_res = kernel::fs::vfs_read(file, nullptr, 0U);
        if (temp_console) {
            (void)kernel::fs::vfs_close(file);
        }
        return (zero_res < 0) ? zero_res : 0;
    }

    char kbuf[READ_CHUNK_SIZE]{};
    size_t total_read = 0U;

    while (total_read < count) {
        const size_t remaining = count - total_read;
        const size_t chunk = (remaining < READ_CHUNK_SIZE) ? remaining : READ_CHUNK_SIZE;
        const int64_t nread = kernel::fs::vfs_read(file, kbuf, chunk);
        if (nread < 0) {
            if (temp_console) {
                (void)kernel::fs::vfs_close(file);
            }
            return (total_read > 0U) ? static_cast<int64_t>(total_read) : nread;
        }
        if (nread == 0) {
            break;
        }
        if (Kernel::MM::copy_to_user(root_paddr, user_buf + total_read, kbuf,
                                     static_cast<size_t>(nread)) < 0) {
            if (temp_console) {
                (void)kernel::fs::vfs_close(file);
            }
            return (total_read > 0U) ? static_cast<int64_t>(total_read) : ERR_EFAULT;
        }
        total_read += static_cast<size_t>(nread);
        if (static_cast<size_t>(nread) < chunk) {
            break;
        }
    }

    if (temp_console) {
        (void)kernel::fs::vfs_close(file);
    }
    return static_cast<int64_t>(total_read);
}

int64_t sys_write(uint64_t raw_fd, uintptr_t user_buf, uint64_t raw_count) noexcept {
    if (raw_count > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }
    const auto count = static_cast<size_t>(raw_count);
    auto* const proc = Kernel::Process::current_process();

    kernel::fs::File* file = nullptr;
    bool temp_console = false;
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        file = get_proc_file(proc, raw_fd);
        if (file == nullptr) {
            return ERR_EBADF;
        }
        root_paddr = proc->address_space.root_paddr();
    } else {
        if (raw_fd != 1U && raw_fd != 2U) {
            return ERR_EBADF;
        }
        root_paddr = Kernel::MM::AddressSpace::current_root_paddr();
        if (kernel::fs::devfs_open_console(kernel::fs::kOpenWriteOnly, &file) != kernel::fs::kOk) {
            return ERR_EBADF;
        }
        temp_console = true;
    }

    if (!validate_user_read_range(root_paddr, user_buf, count)) {
        if (temp_console) {
            (void)kernel::fs::vfs_close(file);
        }
        return ERR_EFAULT;
    }
    if (count == 0U) {
        const int64_t zero_res = kernel::fs::vfs_write(file, nullptr, 0U);
        if (temp_console) {
            (void)kernel::fs::vfs_close(file);
        }
        return (zero_res < 0) ? zero_res : 0;
    }

    char kbuf[WRITE_CHUNK_SIZE]{};
    size_t total_written = 0U;

    while (total_written < count) {
        const size_t remaining = count - total_written;
        const size_t chunk = (remaining < WRITE_CHUNK_SIZE) ? remaining : WRITE_CHUNK_SIZE;
        if (Kernel::MM::copy_from_user(root_paddr, kbuf, user_buf + total_written, chunk) < 0) {
            if (temp_console) {
                (void)kernel::fs::vfs_close(file);
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : ERR_EFAULT;
        }
        const int64_t nwritten = kernel::fs::vfs_write(file, kbuf, chunk);
        if (nwritten < 0) {
            if (temp_console) {
                (void)kernel::fs::vfs_close(file);
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : nwritten;
        }
        total_written += static_cast<size_t>(nwritten);
        if (static_cast<size_t>(nwritten) < chunk) {
            break;
        }
    }

    if (temp_console) {
        (void)kernel::fs::vfs_close(file);
    }
    return static_cast<int64_t>(total_written);
}

int64_t sys_writev(uint64_t raw_fd, uintptr_t user_iov, uint64_t raw_iovcnt) noexcept {
    const auto fd_idx = static_cast<size_t>(raw_fd);
    auto* const proc = Kernel::Process::current_process();
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        if (proc->fd_table == nullptr || fd_idx >= proc->fd_capacity) {
            return ERR_EBADF;
        }
        const auto& desc = proc->fd_table[fd_idx];
        if (!desc.is_open() || !desc.is_writable()) {
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

int16_t poll_fd_revents(const Kernel::Process::Process& proc, int32_t raw_fd,
                        int16_t events) noexcept {
    if (raw_fd < 0) {
        return 0;
    }
    if (proc.fd_table == nullptr || static_cast<size_t>(raw_fd) >= proc.fd_capacity) {
        return POLLNVAL;
    }
    const auto& desc = proc.fd_table[static_cast<size_t>(raw_fd)];
    if (!desc.is_open()) {
        return POLLNVAL;
    }
    return kernel::fs::vfs_poll(desc.file, events);
}

int64_t sys_ppoll(uintptr_t user_fds, uint64_t nfds, uintptr_t user_tmo, uintptr_t user_sigmask,
                  uint64_t sigsetsize) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (nfds > proc->rlim_nofile_cur) {
        return ERR_EINVAL;
    }

    const uintptr_t root_paddr = proc->address_space.root_paddr();
    const auto count = static_cast<size_t>(nfds);

    LinuxPollFd stack_fds[POLL_STACK_THRESHOLD]{};
    LinuxPollFd* heap_fds = nullptr;
    LinuxPollFd* kfdfs = stack_fds;

    struct PollHeapGuard {
        LinuxPollFd* ptr{nullptr};
        explicit PollHeapGuard(LinuxPollFd* heap_ptr) noexcept : ptr(heap_ptr) {}
        PollHeapGuard(const PollHeapGuard&) = delete;
        PollHeapGuard& operator=(const PollHeapGuard&) = delete;
        PollHeapGuard(PollHeapGuard&&) = delete;
        PollHeapGuard& operator=(PollHeapGuard&&) = delete;
        ~PollHeapGuard() noexcept { delete[] ptr; }
    };

    if (count > POLL_STACK_THRESHOLD) {
        heap_fds = new (std::nothrow) LinuxPollFd[count]{};
        if (heap_fds == nullptr) {
            return ERR_ENOMEM;
        }
        kfdfs = heap_fds;
    }
    const PollHeapGuard heap_guard{heap_fds};

    if (count > 0U) {
        if (user_fds == 0U) {
            return ERR_EFAULT;
        }
        if (Kernel::MM::copy_from_user(root_paddr, kfdfs, user_fds, count * sizeof(LinuxPollFd)) <
            0) {
            return ERR_EFAULT;
        }
    }

    bool has_timeout = false;
    bool zero_timeout = false;
    uint64_t deadline_ticks = 0ULL;
    if (user_tmo != 0U) {
        LinuxTimespec tmo{};
        if (Kernel::MM::copy_from_user(root_paddr, &tmo, user_tmo, sizeof(tmo)) < 0) {
            return ERR_EFAULT;
        }
        if (tmo.tv_sec < 0 || tmo.tv_nsec < 0 || tmo.tv_nsec >= 1000000000LL) {
            return ERR_EINVAL;
        }
        has_timeout = true;
        if (tmo.tv_sec == 0 && tmo.tv_nsec == 0) {
            zero_timeout = true;
        } else {
            const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
            deadline_ticks =
                HAL::QemuVirt::GenericTimer::counter_ticks() + timespec_to_timer_ticks(tmo, freq);
        }
    }

    bool swapped_mask = false;
    const uint64_t saved_mask = proc->blocked_signals;
    if (user_sigmask != 0U) {
        if (sigsetsize != sizeof(uint64_t)) {
            return ERR_EINVAL;
        }
        uint64_t new_mask = 0ULL;
        if (Kernel::MM::copy_from_user(root_paddr, &new_mask, user_sigmask, sizeof(new_mask)) < 0) {
            return ERR_EFAULT;
        }
        constexpr uint64_t UNMASKABLE =
            (1ULL << (Kernel::Process::SIG_KILL - 1U)) | (1ULL << (Kernel::Process::SIG_STOP - 1U));
        proc->blocked_signals = new_mask & ~UNMASKABLE;
        swapped_mask = true;
    }

    auto restore_mask_normal = [&]() {
        if (swapped_mask) {
            proc->blocked_signals = saved_mask;
        }
    };

    auto restore_mask_eintr = [&]() {
        if (swapped_mask) {
            proc->has_saved_sigmask = true;
            proc->saved_sigmask = saved_mask;
        }
    };

    while (true) {
        int64_t ready_count = 0;
        for (size_t i = 0U; i < count; ++i) {
            kfdfs[i].revents = poll_fd_revents(*proc, kfdfs[i].fd, kfdfs[i].events);
            if (kfdfs[i].revents != 0) {
                ready_count++;
            }
        }

        if (ready_count > 0 || zero_timeout ||
            (has_timeout && HAL::QemuVirt::GenericTimer::counter_ticks() >= deadline_ticks)) {
            if (count > 0U) {
                if (Kernel::MM::copy_to_user(root_paddr, user_fds, kfdfs,
                                             count * sizeof(LinuxPollFd)) < 0) {
                    restore_mask_normal();
                    return ERR_EFAULT;
                }
            }
            restore_mask_normal();
            return ready_count;
        }

        if (Kernel::Process::has_unblocked_pending_signal(*proc)) {
            restore_mask_eintr();
            return ERR_EINTR;
        }

        wait_for_event_or_tick();
    }
}

int64_t sys_pselect6(int32_t nfds, uintptr_t user_rfds, uintptr_t user_wfds, uintptr_t user_efds,
                     uintptr_t user_tmo, uintptr_t user_sigpack) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (nfds < 0 || static_cast<size_t>(nfds) > MAX_FDSET_FDS) {
        return ERR_EINVAL;
    }

    const uintptr_t root_paddr = proc->address_space.root_paddr();
    const auto u_nfds = static_cast<size_t>(nfds);
    const size_t nwords = (u_nfds + 63U) / 64U;
    const size_t set_bytes = nwords * sizeof(uint64_t);

    uint64_t rfds[MAX_FDSET_WORDS]{};
    uint64_t wfds[MAX_FDSET_WORDS]{};
    uint64_t efds[MAX_FDSET_WORDS]{};

    if (set_bytes > 0U) {
        if (user_rfds != 0U &&
            Kernel::MM::copy_from_user(root_paddr, rfds, user_rfds, set_bytes) < 0) {
            return ERR_EFAULT;
        }
        if (user_wfds != 0U &&
            Kernel::MM::copy_from_user(root_paddr, wfds, user_wfds, set_bytes) < 0) {
            return ERR_EFAULT;
        }
        if (user_efds != 0U &&
            Kernel::MM::copy_from_user(root_paddr, efds, user_efds, set_bytes) < 0) {
            return ERR_EFAULT;
        }
    }

    // Validate all requested file descriptors are open before blocking
    for (size_t fd = 0U; fd < u_nfds; ++fd) {
        const size_t word = fd / 64U;
        const uint64_t bit = 1ULL << (fd % 64U);
        const bool want_any = ((rfds[word] & bit) != 0ULL) || ((wfds[word] & bit) != 0ULL) ||
                              ((efds[word] & bit) != 0ULL);
        if (want_any) {
            if (proc->fd_table == nullptr || fd >= proc->fd_capacity ||
                !proc->fd_table[fd].is_open()) {
                return ERR_EBADF;
            }
        }
    }

    bool has_timeout = false;
    bool zero_timeout = false;
    uint64_t deadline_ticks = 0ULL;
    if (user_tmo != 0U) {
        LinuxTimespec tmo{};
        if (Kernel::MM::copy_from_user(root_paddr, &tmo, user_tmo, sizeof(tmo)) < 0) {
            return ERR_EFAULT;
        }
        if (tmo.tv_sec < 0 || tmo.tv_nsec < 0 || tmo.tv_nsec >= 1000000000LL) {
            return ERR_EINVAL;
        }
        has_timeout = true;
        if (tmo.tv_sec == 0 && tmo.tv_nsec == 0) {
            zero_timeout = true;
        } else {
            const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
            deadline_ticks =
                HAL::QemuVirt::GenericTimer::counter_ticks() + timespec_to_timer_ticks(tmo, freq);
        }
    }

    bool swapped_mask = false;
    const uint64_t saved_mask = proc->blocked_signals;
    if (user_sigpack != 0U) {
        LinuxSigsetArgpack pack{};
        if (Kernel::MM::copy_from_user(root_paddr, &pack, user_sigpack, sizeof(pack)) < 0) {
            return ERR_EFAULT;
        }
        if (pack.ss != 0ULL) {
            if (pack.ss_len != sizeof(uint64_t)) {
                return ERR_EINVAL;
            }
            uint64_t new_mask = 0ULL;
            if (Kernel::MM::copy_from_user(root_paddr, &new_mask, static_cast<uintptr_t>(pack.ss),
                                           sizeof(new_mask)) < 0) {
                return ERR_EFAULT;
            }
            constexpr uint64_t UNMASKABLE = (1ULL << (Kernel::Process::SIG_KILL - 1U)) |
                                            (1ULL << (Kernel::Process::SIG_STOP - 1U));
            proc->blocked_signals = new_mask & ~UNMASKABLE;
            swapped_mask = true;
        }
    }

    auto restore_mask_normal = [&]() {
        if (swapped_mask) {
            proc->blocked_signals = saved_mask;
        }
    };

    auto restore_mask_eintr = [&]() {
        if (swapped_mask) {
            proc->has_saved_sigmask = true;
            proc->saved_sigmask = saved_mask;
        }
    };

    constexpr auto READ_REQ =
        static_cast<int16_t>(static_cast<uint32_t>(POLLIN) | static_cast<uint32_t>(POLLRDNORM));
    constexpr auto READ_READY =
        static_cast<uint16_t>(static_cast<uint32_t>(POLLIN) | static_cast<uint32_t>(POLLRDNORM) |
                              static_cast<uint32_t>(POLLHUP) | static_cast<uint32_t>(POLLERR));
    constexpr auto WRITE_REQ =
        static_cast<int16_t>(static_cast<uint32_t>(POLLOUT) | static_cast<uint32_t>(POLLWRNORM));
    constexpr auto WRITE_READY =
        static_cast<uint16_t>(static_cast<uint32_t>(POLLOUT) | static_cast<uint32_t>(POLLWRNORM) |
                              static_cast<uint32_t>(POLLERR));
    constexpr auto ERR_READY = static_cast<uint16_t>(POLLERR);

    while (true) {
        uint64_t out_rfds[MAX_FDSET_WORDS]{};
        uint64_t out_wfds[MAX_FDSET_WORDS]{};
        uint64_t out_efds[MAX_FDSET_WORDS]{};
        int64_t ready_bits = 0;

        for (size_t fd = 0U; fd < u_nfds; ++fd) {
            const size_t word = fd / 64U;
            const uint64_t bit = 1ULL << (fd % 64U);

            if ((rfds[word] & bit) != 0ULL) {
                const auto rev = static_cast<uint16_t>(
                    poll_fd_revents(*proc, static_cast<int32_t>(fd), READ_REQ));
                if ((rev & READ_READY) != 0U) {
                    out_rfds[word] |= bit;
                    ready_bits++;
                }
            }
            if ((wfds[word] & bit) != 0ULL) {
                const auto rev = static_cast<uint16_t>(
                    poll_fd_revents(*proc, static_cast<int32_t>(fd), WRITE_REQ));
                if ((rev & WRITE_READY) != 0U) {
                    out_wfds[word] |= bit;
                    ready_bits++;
                }
            }
            if ((efds[word] & bit) != 0ULL) {
                const auto rev =
                    static_cast<uint16_t>(poll_fd_revents(*proc, static_cast<int32_t>(fd), 0));
                if ((rev & ERR_READY) != 0U) {
                    out_efds[word] |= bit;
                    ready_bits++;
                }
            }
        }

        if (ready_bits > 0 || zero_timeout ||
            (has_timeout && HAL::QemuVirt::GenericTimer::counter_ticks() >= deadline_ticks)) {
            if (set_bytes > 0U) {
                if (user_rfds != 0U &&
                    Kernel::MM::copy_to_user(root_paddr, user_rfds, out_rfds, set_bytes) < 0) {
                    restore_mask_normal();
                    return ERR_EFAULT;
                }
                if (user_wfds != 0U &&
                    Kernel::MM::copy_to_user(root_paddr, user_wfds, out_wfds, set_bytes) < 0) {
                    restore_mask_normal();
                    return ERR_EFAULT;
                }
                if (user_efds != 0U &&
                    Kernel::MM::copy_to_user(root_paddr, user_efds, out_efds, set_bytes) < 0) {
                    restore_mask_normal();
                    return ERR_EFAULT;
                }
            }
            restore_mask_normal();
            return ready_bits;
        }

        if (Kernel::Process::has_unblocked_pending_signal(*proc)) {
            restore_mask_eintr();
            return ERR_EINTR;
        }

        wait_for_event_or_tick();
    }
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
    for (auto* proc = Kernel::Process::process_list_head(); proc != nullptr;
         proc = proc->global_next) {
        if (proc->state == Kernel::Process::ProcessState::Unused ||
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
// Process Lifecycle, Memory Management, Resource Limits & System Identity
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

int64_t sys_umask(uint64_t mask) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return 0022;
    }
    const uint32_t old_mask = proc->umask;
    proc->umask = static_cast<uint32_t>(mask) & 0777U;
    return static_cast<int64_t>(old_mask);
}

int64_t sys_prlimit64(int32_t pid, uint32_t resource, uintptr_t user_new_rlim,
                      uintptr_t user_old_rlim) noexcept {
    auto* const caller = Kernel::Process::current_process();
    if (caller == nullptr) {
        return ERR_EINVAL;
    }
    if (pid < 0 || resource >= Kernel::Process::RLIM_NLIMITS) {
        return ERR_EINVAL;
    }

    Kernel::Process::Process* target = caller;
    if (pid > 0 && static_cast<uint32_t>(pid) != caller->pid) {
        target = Kernel::Process::find_process_by_pid(static_cast<uint32_t>(pid));
        if (target == nullptr || target->state == Kernel::Process::ProcessState::Zombie) {
            return ERR_ESRCH;
        }
    }

    LinuxRlimit64 current_rlim{
        .rlim_cur = Kernel::Process::RLIM64_INFINITY,
        .rlim_max = Kernel::Process::RLIM64_INFINITY,
    };

    switch (resource) {
    case Kernel::Process::RLIMIT_NOFILE:
        current_rlim.rlim_cur = target->rlim_nofile_cur;
        current_rlim.rlim_max = target->rlim_nofile_max;
        break;
    case Kernel::Process::RLIMIT_STACK:
        current_rlim.rlim_cur = Kernel::Process::DEFAULT_USER_STACK_SIZE;
        current_rlim.rlim_max = Kernel::Process::DEFAULT_USER_STACK_SIZE;
        break;
    case Kernel::Process::RLIMIT_NPROC:
        current_rlim.rlim_cur = target->rlim_nproc_cur;
        current_rlim.rlim_max = target->rlim_nproc_max;
        break;
    case Kernel::Process::RLIMIT_CORE:
        current_rlim.rlim_cur = 0ULL;
        current_rlim.rlim_max = 0ULL;
        break;
    default:
        break;
    }

    if (user_old_rlim != 0U) {
        if (Kernel::MM::copy_to_user(caller->address_space, user_old_rlim, &current_rlim,
                                     sizeof(current_rlim)) < 0) {
            return ERR_EFAULT;
        }
    }

    if (user_new_rlim != 0U) {
        LinuxRlimit64 new_rlim{};
        if (Kernel::MM::copy_from_user(caller->address_space, &new_rlim, user_new_rlim,
                                       sizeof(new_rlim)) < 0) {
            return ERR_EFAULT;
        }
        if (new_rlim.rlim_cur > new_rlim.rlim_max) {
            return ERR_EINVAL;
        }
        if (resource == Kernel::Process::RLIMIT_NOFILE) {
            if (new_rlim.rlim_max > target->rlim_nofile_max ||
                new_rlim.rlim_max > Kernel::Process::DEFAULT_RLIMIT_NOFILE_MAX) {
                return ERR_EPERM;
            }
            target->rlim_nofile_cur = new_rlim.rlim_cur;
            target->rlim_nofile_max = new_rlim.rlim_max;
        } else if (resource == Kernel::Process::RLIMIT_NPROC) {
            if (new_rlim.rlim_max > target->rlim_nproc_max) {
                return ERR_EPERM;
            }
            target->rlim_nproc_cur = new_rlim.rlim_cur;
            target->rlim_nproc_max = new_rlim.rlim_max;
        }
    }

    return 0;
}

int64_t sys_getrusage(int32_t who, uintptr_t user_usage) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) {
        return ERR_EINVAL;
    }

    const uint64_t uticks = (who == RUSAGE_CHILDREN) ? proc->cutime : proc->utime;
    const uint64_t sticks = (who == RUSAGE_CHILDREN) ? proc->cstime : proc->stime;
    // Timer runs at 100 Hz (10,000 microseconds per tick)
    const uint64_t user_usec = uticks * 10000ULL;
    const uint64_t sys_usec = sticks * 10000ULL;

    LinuxRusage usage{};
    usage.ru_utime.tv_sec = static_cast<int64_t>(user_usec / 1000000ULL);
    usage.ru_utime.tv_usec = static_cast<int64_t>(user_usec % 1000000ULL);
    usage.ru_stime.tv_sec = static_cast<int64_t>(sys_usec / 1000000ULL);
    usage.ru_stime.tv_usec = static_cast<int64_t>(sys_usec % 1000000ULL);

    return Kernel::MM::copy_to_user(proc->address_space, user_usage, &usage, sizeof(usage));
}

int64_t sys_getrandom(uintptr_t user_buf, uint64_t raw_buflen, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) != 0ULL) {
        return ERR_EINVAL;
    }
    if (raw_buflen > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }

    const auto buflen = static_cast<size_t>(raw_buflen);
    const uintptr_t root_paddr = proc->address_space.root_paddr();
    if (!validate_user_write_range(root_paddr, user_buf, buflen)) {
        return ERR_EFAULT;
    }
    if (buflen == 0U) {
        return 0;
    }

    static uint64_t entropy_state = 0x243F6A8885A308D3ULL;
    entropy_state ^= HAL::QemuVirt::GenericTimer::counter_ticks();
    entropy_state ^= static_cast<uint64_t>(proc->pid) << 32U;

    uint8_t kbuf[256]{};
    size_t written = 0U;

    while (written < buflen) {
        const size_t chunk =
            ((buflen - written) < sizeof(kbuf)) ? (buflen - written) : sizeof(kbuf);
        for (size_t i = 0U; i < chunk; i += 8U) {
            entropy_state += 0x9E3779B97F4A7C15ULL;
            uint64_t mixed = entropy_state;
            mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
            mixed = mixed ^ (mixed >> 31U);

            const size_t rem = chunk - i;
            const size_t bytes = (rem < 8U) ? rem : 8U;
            for (size_t byte_idx = 0U; byte_idx < bytes; ++byte_idx) {
                kbuf[i + byte_idx] = static_cast<uint8_t>((mixed >> (byte_idx * 8U)) & 0xFFULL);
            }
        }

        if (Kernel::MM::copy_to_user(root_paddr, user_buf + written, kbuf, chunk) < 0) {
            return (written > 0U) ? static_cast<int64_t>(written) : ERR_EFAULT;
        }
        written += chunk;
    }

    return static_cast<int64_t>(written);
}

int64_t sys_nanosleep(uintptr_t user_req, uintptr_t user_rem) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    LinuxTimespec req{};
    if (Kernel::MM::copy_from_user(proc->address_space, &req, user_req, sizeof(req)) < 0) {
        return ERR_EFAULT;
    }
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000LL) {
        return ERR_EINVAL;
    }

    const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
    const uint64_t start_ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
    const uint64_t wait_ticks = timespec_to_timer_ticks(req, freq);

    while (true) {
        const uint64_t now_ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
        const uint64_t elapsed = now_ticks - start_ticks;
        if (elapsed >= wait_ticks) {
            break;
        }

        if (Kernel::Process::has_unblocked_pending_signal(*proc)) {
            if (user_rem != 0U && freq > 0ULL) {
                const uint64_t rem_ticks = wait_ticks - elapsed;
                const LinuxTimespec rem{
                    .tv_sec = static_cast<int64_t>(rem_ticks / freq),
                    .tv_nsec = static_cast<int64_t>(((rem_ticks % freq) * 1000000000ULL) / freq),
                };
                if (Kernel::MM::copy_to_user(proc->address_space, user_rem, &rem, sizeof(rem)) <
                    0) {
                    return ERR_EFAULT;
                }
            }
            return ERR_EINTR;
        }

        wait_for_event_or_tick();
    }

    if (user_rem != 0U) {
        constexpr LinuxTimespec zero_rem{.tv_sec = 0, .tv_nsec = 0};
        if (Kernel::MM::copy_to_user(proc->address_space, user_rem, &zero_rem, sizeof(zero_rem)) <
            0) {
            return ERR_EFAULT;
        }
    }

    return 0;
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
    if (proc->mmap_next_vaddr != 0U && proc->mmap_next_vaddr < brk_ceiling) {
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
            if (!proc->address_space.map_demand_zero(page_vaddr, heap_flags)) {
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

int64_t sys_times(uintptr_t user_buf) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    if (user_buf != 0U) {
        const LinuxTms tms_val{
            .tms_utime = static_cast<int64_t>(proc->utime),
            .tms_stime = static_cast<int64_t>(proc->stime),
            .tms_cutime = static_cast<int64_t>(proc->cutime),
            .tms_cstime = static_cast<int64_t>(proc->cstime),
        };
        if (Kernel::MM::copy_to_user(proc->address_space, user_buf, &tms_val, sizeof(tms_val)) <
            0) {
            return ERR_EFAULT;
        }
    }

    return static_cast<int64_t>(Kernel::Scheduler::timer_ticks());
}

int64_t sys_mmap(uintptr_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t raw_fd,
                 uint64_t offset) noexcept {
    constexpr uintptr_t HIGH_MMAP_ARENA_FLOOR = 0x0000000080000000ULL;
    constexpr uintptr_t HIGH_MMAP_SEARCH_STEP = 0x0000000000200000ULL; // 2 MiB L2 step

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

    auto is_valid_mmap_span = [&](uintptr_t start, uintptr_t end) noexcept -> bool {
        const bool in_lower = (start >= MMAP_ARENA_FLOOR && end <= stack_floor);
        const bool in_high = (start >= HIGH_MMAP_ARENA_FLOOR && end <= va_limit);
        return in_lower || in_high;
    };

    if (is_fixed) {
        if (addr == 0U || (addr & page_mask) != 0U || addr >= va_limit ||
            length > (va_limit - addr)) {
            return ERR_EINVAL;
        }
        map_start = addr;
        map_end = addr + static_cast<uintptr_t>(length);
        if (!is_valid_mmap_span(map_start, map_end)) {
            return ERR_ENOMEM;
        }
        unmap_and_free_range(proc->address_space, map_start, map_end, page_size);
    } else if (addr != 0U && (addr & page_mask) == 0U && addr < va_limit &&
               length <= (va_limit - addr) &&
               is_valid_mmap_span(addr, addr + static_cast<uintptr_t>(length))) {
        bool hint_free = true;
        for (uintptr_t page = addr; page < addr + static_cast<uintptr_t>(length);
             page += page_size) {
            uintptr_t tmp_pa = 0U;
            HAL::PageFlags tmp_fl = HAL::PageFlags::None;
            uint64_t tmp_pte = 0ULL;
            if (proc->address_space.query_user_page(page, tmp_pa, tmp_fl) ||
                proc->address_space.query_pte_flags(page, tmp_pte)) {
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
        if (length <= arena_top &&
            (arena_top - static_cast<uintptr_t>(length)) >= MMAP_ARENA_FLOOR) {
            const uintptr_t candidate_start = arena_top - static_cast<uintptr_t>(length);
            if (candidate_start > proc->brk_current) {
                map_start = candidate_start;
                map_end = arena_top;
                used_arena_bump = true;
            }
        }
    }

    if (map_start == 0U && va_limit > HIGH_MMAP_ARENA_FLOOR &&
        length <= (va_limit - HIGH_MMAP_ARENA_FLOOR)) {
        const uintptr_t max_start = va_limit - static_cast<uintptr_t>(length);
        for (uintptr_t cand = HIGH_MMAP_ARENA_FLOOR; cand <= max_start;
             cand += HIGH_MMAP_SEARCH_STEP) {
            bool range_free = true;
            for (uintptr_t probe = cand; probe < cand + static_cast<uintptr_t>(length);
                 probe += page_size) {
                uintptr_t tmp_pa = 0U;
                HAL::PageFlags tmp_fl = HAL::PageFlags::None;
                uint64_t tmp_pte = 0ULL;
                if (proc->address_space.query_user_page(probe, tmp_pa, tmp_fl) ||
                    proc->address_space.query_pte_flags(probe, tmp_pte)) {
                    range_free = false;
                    cand = probe & ~(HIGH_MMAP_SEARCH_STEP - 1U);
                    break;
                }
            }
            if (range_free) {
                map_start = cand;
                map_end = cand + static_cast<uintptr_t>(length);
                break;
            }
        }
    }

    if (map_start == 0U) {
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

    for (uintptr_t page_vaddr = map_start; page_vaddr < map_end; page_vaddr += page_size) {
        if (!proc->address_space.map_demand_zero(page_vaddr, page_flags)) {
            unmap_and_free_range(proc->address_space, map_start, page_vaddr, page_size);
            return ERR_ENOMEM;
        }
    }

    if (used_arena_bump) {
        proc->mmap_next_vaddr = map_start;
    }
    return static_cast<int64_t>(map_start);
}

int64_t sys_munmap(uintptr_t addr, uint64_t length) noexcept {
    constexpr uintptr_t HIGH_MMAP_ARENA_FLOOR = 0x0000000080000000ULL;

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
    const bool in_lower_arena = (addr >= MMAP_ARENA_FLOOR && end_addr <= stack_floor);
    const bool in_high_arena = (addr >= HIGH_MMAP_ARENA_FLOOR && end_addr <= va_limit);
    if (!in_lower_arena && !in_high_arena) {
        return ERR_EINVAL;
    }

    unmap_and_free_range(proc->address_space, addr, end_addr, page_size);

    if (in_lower_arena && addr == proc->mmap_next_vaddr) {
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

    if (Kernel::Process::active_process_count() >= parent->rlim_nproc_cur) {
        return ERR_EAGAIN;
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
    child->umask = parent->umask;
    child->rlim_nofile_cur = parent->rlim_nofile_cur;
    child->rlim_nofile_max = parent->rlim_nofile_max;
    child->rlim_nproc_cur = parent->rlim_nproc_cur;
    child->rlim_nproc_max = parent->rlim_nproc_max;
    child->entry_point = parent->entry_point;
    child->brk_start = parent->brk_start;
    child->brk_current = parent->brk_current;
    child->mmap_next_vaddr = parent->mmap_next_vaddr;
    child->user_stack = Kernel::Process::UserStack{};
    child->blocked_signals = parent->blocked_signals;
    cstr_copy(child->cwd, parent->cwd, sizeof(child->cwd));
    cstr_copy(child->exe_path, parent->exe_path, sizeof(child->exe_path));

    for (size_t i = 0U; i < Kernel::Process::MAX_SIGNALS; ++i) {
        child->sigactions[i] = parent->sigactions[i];
    }

    if (parent->fd_table != nullptr && parent->fd_capacity > 0U) {
        if (parent->fd_capacity > child->fd_capacity) {
            if (!Kernel::Process::ensure_fd_capacity(*child, parent->fd_capacity)) {
                Kernel::Process::release_process(*child);
                return ERR_ENOMEM;
            }
        }
        for (size_t idx = 0U; idx < parent->fd_capacity; ++idx) {
            Kernel::Process::copy_fd_slot(child->fd_table[idx], parent->fd_table[idx], false);
        }
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

    const char* argv_kptrs[EXEC_MAX_ARGS + 3U]{};
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

    auto open_and_read_exec = [&](const char* path, uintptr_t& out_buf_paddr, size_t& out_num_pages,
                                  size_t& out_file_size) -> int64_t {
        out_buf_paddr = 0U;
        out_num_pages = 0U;
        out_file_size = 0U;

        kernel::fs::File* exec_file = nullptr;
        const int32_t orc = kernel::fs::vfs_open(kernel::fs::StringView(path),
                                                 kernel::fs::kOpenReadOnly, 0U, &exec_file);
        if (orc != kernel::fs::kOk) {
            return orc;
        }
        if (exec_file->inode == nullptr || !exec_file->inode->is_reg() ||
            exec_file->inode->size == 0U) {
            (void)kernel::fs::vfs_close(exec_file);
            return ERR_ENOEXEC;
        }

        const auto file_size = static_cast<size_t>(exec_file->inode->size);
        const size_t num_pages = (file_size + page_size - 1U) / page_size;
        const uintptr_t buf_paddr = alloc_frames(num_pages);
        if (buf_paddr == 0U) {
            (void)kernel::fs::vfs_close(exec_file);
            return ERR_ENOMEM;
        }

        auto* const dst = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(buf_paddr));
        size_t total = 0U;
        while (total < file_size) {
            const int64_t nread = kernel::fs::vfs_read(exec_file, dst + total, file_size - total);
            if (nread <= 0) {
                free_frames(buf_paddr, num_pages);
                (void)kernel::fs::vfs_close(exec_file);
                return (nread < 0) ? nread : ERR_ENOEXEC;
            }
            total += static_cast<size_t>(nread);
        }
        (void)kernel::fs::vfs_close(exec_file);

        out_buf_paddr = buf_paddr;
        out_num_pages = num_pages;
        out_file_size = file_size;
        return 0;
    };

    uintptr_t exec_buf_paddr = 0U;
    size_t exec_buf_pages = 0U;
    size_t exec_file_size = 0U;
    const int64_t read_exec_res =
        open_and_read_exec(canon_path, exec_buf_paddr, exec_buf_pages, exec_file_size);
    if (read_exec_res < 0) {
        free_frame(scratch_paddr);
        return read_exec_res;
    }

    const auto* exec_bytes =
        reinterpret_cast<const uint8_t*>(Kernel::MM::phys_to_virt(exec_buf_paddr));

    // Handle '#!' shebang scripts (e.g., #!/bin/bash or #!/bin/sh)
    char shebang_interp[Kernel::Process::MAX_PATH_LEN]{};
    char shebang_arg[Kernel::Process::MAX_PATH_LEN]{};
    bool has_shebang_arg = false;
    char exec_binary_path[Kernel::Process::MAX_PATH_LEN]{};
    cstr_copy(exec_binary_path, canon_path, sizeof(exec_binary_path));

    if (exec_file_size >= 2U && exec_bytes[0] == '#' && exec_bytes[1] == '!') {
        size_t pos = 2U;
        while (pos < exec_file_size && (exec_bytes[pos] == ' ' || exec_bytes[pos] == '\t')) {
            pos++;
        }
        size_t interp_len = 0U;
        while (pos < exec_file_size && exec_bytes[pos] != ' ' && exec_bytes[pos] != '\t' &&
               exec_bytes[pos] != '\n' && exec_bytes[pos] != '\r') {
            if (interp_len + 1U >= sizeof(shebang_interp)) {
                free_frames(exec_buf_paddr, exec_buf_pages);
                free_frame(scratch_paddr);
                return ERR_ENOEXEC;
            }
            shebang_interp[interp_len++] = static_cast<char>(exec_bytes[pos++]);
        }
        shebang_interp[interp_len] = '\0';
        if (interp_len == 0U) {
            free_frames(exec_buf_paddr, exec_buf_pages);
            free_frame(scratch_paddr);
            return ERR_ENOEXEC;
        }

        while (pos < exec_file_size && (exec_bytes[pos] == ' ' || exec_bytes[pos] == '\t')) {
            pos++;
        }
        size_t arg_len = 0U;
        while (pos < exec_file_size && exec_bytes[pos] != '\n' && exec_bytes[pos] != '\r') {
            if (arg_len + 1U < sizeof(shebang_arg)) {
                shebang_arg[arg_len++] = static_cast<char>(exec_bytes[pos]);
            }
            pos++;
        }
        while (arg_len > 0U &&
               (shebang_arg[arg_len - 1U] == ' ' || shebang_arg[arg_len - 1U] == '\t')) {
            arg_len--;
        }
        shebang_arg[arg_len] = '\0';
        has_shebang_arg = (arg_len > 0U);

        free_frames(exec_buf_paddr, exec_buf_pages);
        exec_buf_paddr = 0U;

        if (!normalize_vfs_path("/", shebang_interp, exec_binary_path, sizeof(exec_binary_path))) {
            free_frame(scratch_paddr);
            return ERR_ENOENT;
        }
        const int64_t interp_res =
            open_and_read_exec(exec_binary_path, exec_buf_paddr, exec_buf_pages, exec_file_size);
        if (interp_res < 0) {
            free_frame(scratch_paddr);
            return interp_res;
        }
        exec_bytes = reinterpret_cast<const uint8_t*>(Kernel::MM::phys_to_virt(exec_buf_paddr));

        const int extra = has_shebang_arg ? 2 : 1;
        if (static_cast<size_t>(argc) + static_cast<size_t>(extra) > EXEC_MAX_ARGS) {
            free_frames(exec_buf_paddr, exec_buf_pages);
            free_frame(scratch_paddr);
            return ERR_E2BIG;
        }
        for (int idx = argc; idx >= 1; --idx) {
            argv_kptrs[idx + extra] = argv_kptrs[idx];
        }
        argv_kptrs[0] = exec_binary_path;
        if (has_shebang_arg) {
            argv_kptrs[1] = shebang_arg;
            argv_kptrs[2] = canon_path;
        } else {
            argv_kptrs[1] = canon_path;
        }
        argc += extra;
    }

    auto load_res = Kernel::Loader::load_elf(exec_bytes, exec_file_size);
    free_frames(exec_buf_paddr, exec_buf_pages);
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
        if (!loaded.address_space.is_valid() && new_stack.phys_base != 0U &&
            new_stack.num_pages > 0U) {
            free_frames(new_stack.phys_base, new_stack.num_pages);
        }
        loaded.address_space.destroy();
        free_frame(scratch_paddr);
        return ERR_E2BIG;
    }

    if (proc->fd_table == nullptr &&
        !Kernel::Process::init_fd_table(*proc, Kernel::Process::INITIAL_FD_CAPACITY)) {
        if (!loaded.address_space.is_valid() && new_stack.phys_base != 0U &&
            new_stack.num_pages > 0U) {
            free_frames(new_stack.phys_base, new_stack.num_pages);
        }
        loaded.address_space.destroy();
        free_frame(scratch_paddr);
        return ERR_ENOMEM;
    }

    loaded.address_space.activate();

    if (!proc->address_space.is_valid() && proc->user_stack.phys_base != 0U &&
        proc->user_stack.num_pages > 0U) {
        free_frames(proc->user_stack.phys_base, proc->user_stack.num_pages);
    }
    proc->address_space = static_cast<Kernel::MM::AddressSpace&&>(loaded.address_space);
    cstr_copy(proc->exe_path, exec_binary_path, sizeof(proc->exe_path));

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

    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();

    Kernel::Process::Process* target_child = nullptr;
    int32_t encoded_wstatus = 0;
    bool reap_child = false;

    while (true) {
        cpu.disable_interrupts();

        bool has_matching_child = false;
        Kernel::Process::Process* ready_child = nullptr;

        for (auto* candidate = Kernel::Process::process_list_head(); candidate != nullptr;
             candidate = candidate->global_next) {
            if (candidate->state == Kernel::Process::ProcessState::Unused) {
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
                ready_child = candidate;
                encoded_wstatus = static_cast<int32_t>(
                    (static_cast<uint32_t>(candidate->exit_status) & 0xFFU) << 8U);
                reap_child = true;
                break;
            }
            if ((options & WAIT_WUNTRACED) != 0ULL &&
                candidate->state == Kernel::Process::ProcessState::Stopped &&
                !candidate->stop_reported) {
                ready_child = candidate;
                encoded_wstatus =
                    static_cast<int32_t>(((candidate->stop_signal & 0xFFU) << 8U) | 0x7FU);
                candidate->stop_reported = true;
                reap_child = false;
                break;
            }
            if ((options & WAIT_WCONTINUED) != 0ULL && candidate->continued) {
                ready_child = candidate;
                encoded_wstatus = 0xFFFF;
                candidate->continued = false;
                reap_child = false;
                break;
            }
        }

        if (!has_matching_child) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return ERR_ECHILD;
        }

        if (ready_child != nullptr) {
            target_child = ready_child;
            break;
        }

        if ((options & WAIT_WNOHANG) != 0ULL) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return 0;
        }

        if (Kernel::Process::has_unblocked_pending_signal(*parent)) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return ERR_EINTR;
        }

        parent->child_wait_queue.sleep();
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }

    if (user_wstatus != 0U) {
        const int64_t status_res = Kernel::MM::copy_to_user(
            parent->address_space, user_wstatus, &encoded_wstatus, sizeof(encoded_wstatus));
        if (status_res < 0) {
            return ERR_EFAULT;
        }
    }

    if (user_rusage != 0U) {
        LinuxRusage rusage{};
        const uint64_t child_user_usec =
            static_cast<uint64_t>(target_child->utime + target_child->cutime) * 10000ULL;
        const uint64_t child_sys_usec =
            static_cast<uint64_t>(target_child->stime + target_child->cstime) * 10000ULL;
        rusage.ru_utime.tv_sec = static_cast<int64_t>(child_user_usec / 1000000ULL);
        rusage.ru_utime.tv_usec = static_cast<int64_t>(child_user_usec % 1000000ULL);
        rusage.ru_stime.tv_sec = static_cast<int64_t>(child_sys_usec / 1000000ULL);
        rusage.ru_stime.tv_usec = static_cast<int64_t>(child_sys_usec % 1000000ULL);
        const int64_t rusage_res =
            Kernel::MM::copy_to_user(parent->address_space, user_rusage, &rusage, sizeof(rusage));
        if (rusage_res < 0) {
            return ERR_EFAULT;
        }
    }

    const uint32_t reported_pid = target_child->pid;
    if (reap_child) {
        parent->cutime += target_child->utime + target_child->cutime;
        parent->cstime += target_child->stime + target_child->cstime;
        Kernel::Process::release_process(*target_child);
    }
    return static_cast<int64_t>(reported_pid);
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

namespace Kernel::Process {

void set_foreground_pgid(uint32_t pgid) noexcept {
    kernel::fs::console_set_foreground_pgid(pgid);
}

uint32_t get_foreground_pgid() noexcept {
    return kernel::fs::console_get_foreground_pgid();
}

} // namespace Kernel::Process

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

        case SYS_FACCESSAT2:
            ret_val = sys_faccessat2(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_CHDIR:
            ret_val = sys_chdir(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;
        case SYS_MKDIRAT:
            ret_val = sys_mkdirat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_UNLINKAT:
            ret_val = sys_unlinkat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_RENAMEAT:
            ret_val = sys_renameat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 2U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)));
            break;

        case SYS_RENAMEAT2:
            ret_val = sys_renameat2(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 2U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)), cpu.get_syscall_arg(ctx, 4U));
            break;

        case SYS_TRUNCATE:
            ret_val = sys_truncate(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                   static_cast<int64_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_FTRUNCATE:
            ret_val = sys_ftruncate(cpu.get_syscall_arg(ctx, 0U),
                                    static_cast<int64_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_FCHMOD:
            ret_val = sys_fchmod(cpu.get_syscall_arg(ctx, 0U), cpu.get_syscall_arg(ctx, 1U));
            break;

        case SYS_FCHMODAT:
            ret_val = sys_fchmodat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_FCHOWNAT:
            ret_val = sys_fchownat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 2U)),
                static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 3U)), cpu.get_syscall_arg(ctx, 4U));
            break;

        case SYS_FCHOWN:
            ret_val = sys_fchown(cpu.get_syscall_arg(ctx, 0U),
                                 static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 1U)),
                                 static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 2U)));
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

        case SYS_PSELECT6:
            ret_val = sys_pselect6(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 4U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 5U)));
            break;

        case SYS_PPOLL:
            ret_val = sys_ppoll(
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)), cpu.get_syscall_arg(ctx, 1U),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)), cpu.get_syscall_arg(ctx, 4U));
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

        case SYS_FSYNC:
            ret_val = sys_fsync(cpu.get_syscall_arg(ctx, 0U), 0);
            break;

        case SYS_FDATASYNC:
            ret_val = sys_fsync(cpu.get_syscall_arg(ctx, 0U), 1);
            break;

        case SYS_UTIMENSAT:
            ret_val = sys_utimensat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)), cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            sys_exit(cpu.get_syscall_arg(ctx, 0U));

        case SYS_SET_TID_ADDRESS:
            ret_val = sys_set_tid_address(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_NANOSLEEP:
            ret_val = sys_nanosleep(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                    static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)));
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

        case SYS_TIMES:
            ret_val = sys_times(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
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

        case SYS_GETRUSAGE:
            ret_val = sys_getrusage(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_UMASK:
            ret_val = sys_umask(cpu.get_syscall_arg(ctx, 0U));
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

        case SYS_PRLIMIT64:
            ret_val = sys_prlimit64(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)));
            break;

        case SYS_GETRANDOM:
            ret_val = sys_getrandom(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
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