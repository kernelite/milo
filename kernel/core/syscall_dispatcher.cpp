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
#include "process/resource.hpp"
#include "process/scheduler.hpp"
#include "process/stack_builder.hpp"

#include <cstddef>
#include <cstdint>
#include <new>

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
constexpr uint64_t SYS_PSELECT6 = 72U;
constexpr uint64_t SYS_PPOLL = 73U;
constexpr uint64_t SYS_READLINKAT = 78U;
constexpr uint64_t SYS_NEWFSTATAT = 79U;
constexpr uint64_t SYS_FSTAT = 80U;
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
constexpr int64_t ERR_ENOTDIR = -20;
constexpr int64_t ERR_EISDIR = -21;
constexpr int64_t ERR_EINVAL = -22;
constexpr int64_t ERR_ENOTTY = -25;
constexpr int64_t ERR_ESPIPE = -29;
constexpr int64_t ERR_EROFS = -30;
constexpr int64_t ERR_ERANGE = -34;
constexpr int64_t ERR_ENOSYS = -38;

// File / Open / Fcntl Constants
constexpr int32_t AT_FDCWD = -100;
constexpr uint64_t AT_SYMLINK_NOFOLLOW = 0x100ULL;
constexpr uint64_t AT_EACCESS = 0x200ULL;
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

// Terminal ioctl Request Codes
constexpr uint64_t IOCTL_TCGETS = 0x5401U;
constexpr uint64_t IOCTL_TCSETS = 0x5402U;
constexpr uint64_t IOCTL_TCSETSW = 0x5403U;
constexpr uint64_t IOCTL_TCSETSF = 0x5404U;
constexpr uint64_t IOCTL_TIOCGPGRP = 0x540FU;
constexpr uint64_t IOCTL_TIOCSPGRP = 0x5410U;
constexpr uint64_t IOCTL_TIOCGWINSZ = 0x5413U;

// termios Flag Bits
constexpr uint32_t TERMIOS_INLCR = 0x0040U;
constexpr uint32_t TERMIOS_IGNCR = 0x0080U;
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

// termios c_cc Indices (Linux asm-generic)
constexpr size_t CC_VINTR = 0U;
constexpr size_t CC_VQUIT = 1U;
constexpr size_t CC_VERASE = 2U;
constexpr size_t CC_VKILL = 3U;
constexpr size_t CC_VEOF = 4U;
constexpr size_t CC_VTIME = 5U;
constexpr size_t CC_VMIN = 6U;

constexpr size_t TERMIOS_NCCS = 19U;
constexpr uint16_t DEFAULT_TTY_ROWS = 24U;
constexpr uint16_t DEFAULT_TTY_COLS = 80U;

constexpr size_t READ_CHUNK_SIZE = 256U;
constexpr size_t WRITE_CHUNK_SIZE = 256U;
constexpr size_t TTY_RX_BUF_SIZE = 256U;

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
uint8_t g_tty_rx_buf[TTY_RX_BUF_SIZE]{};
size_t g_tty_rx_head = 0U;
size_t g_tty_rx_tail = 0U;
size_t g_tty_rx_count = 0U;

void tty_flush_rx_unlocked() noexcept {
    g_tty_rx_head = 0U;
    g_tty_rx_tail = 0U;
    g_tty_rx_count = 0U;
}

void tty_pump_uart_unlocked() noexcept {
    auto& console = HAL::get_console();
    while (console.has_input() && g_tty_rx_count < TTY_RX_BUF_SIZE) {
        const uint8_t chr = console.getc();
        if ((g_console_termios.c_lflag & TERMIOS_ISIG) != 0U) {
            if (chr == g_console_termios.c_cc[CC_VINTR]) {
                if ((g_console_termios.c_lflag & TERMIOS_ECHO) != 0U) {
                    console.write("^C\r\n", 4U);
                }
                tty_flush_rx_unlocked();
                Kernel::Process::signal_process_group(g_foreground_pgid, Kernel::Process::SIG_INT);
                continue;
            }
            if (chr == g_console_termios.c_cc[CC_VQUIT]) {
                if ((g_console_termios.c_lflag & TERMIOS_ECHO) != 0U) {
                    console.write("^\\\r\n", 4U);
                }
                tty_flush_rx_unlocked();
                Kernel::Process::signal_process_group(g_foreground_pgid, Kernel::Process::SIG_QUIT);
                continue;
            }
        }
        g_tty_rx_buf[g_tty_rx_tail] = chr;
        g_tty_rx_tail = (g_tty_rx_tail + 1U) % TTY_RX_BUF_SIZE;
        g_tty_rx_count++;
    }
}

size_t tty_rx_available() noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    tty_pump_uart_unlocked();
    const size_t avail = g_tty_rx_count;

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
    return avail;
}

bool tty_try_pop_byte(uint8_t& out_byte) noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    tty_pump_uart_unlocked();
    bool got_byte = false;
    if (g_tty_rx_count > 0U) {
        out_byte = g_tty_rx_buf[g_tty_rx_head];
        g_tty_rx_head = (g_tty_rx_head + 1U) % TTY_RX_BUF_SIZE;
        g_tty_rx_count--;
        got_byte = true;
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
    return got_byte;
}

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
    } else if (desc.type == Kernel::Process::FdType::DevNull) {
        stat_out.st_ino = 4ULL;
        stat_out.st_mode = 0020666U; // S_IFCHR | 0666
        stat_out.st_rdev = 0x0103ULL;
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

/* Remove this if everything works without it:
int allocate_fd_slot(Kernel::Process::Process& proc, size_t min_fd = 0U) noexcept {
    size_t max_fds = Kernel::Process::MAX_FDS_PER_PROCESS;
    if (proc.rlim_nofile_cur < max_fds) {
        max_fds = static_cast<size_t>(proc.rlim_nofile_cur);
    }
    for (size_t idx = min_fd; idx < max_fds; ++idx) {
        if (!proc.fd_table[idx].in_use) {
            return static_cast<int>(idx);
        }
    }
    return -1;
}
*/

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
    if (dirfd < 0 || proc.fd_table == nullptr || static_cast<size_t>(dirfd) >= proc.fd_capacity) {
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

    const uint64_t accmode = flags & O_ACCMODE;
    uint32_t rw_flags = 0U;
    if (accmode == O_RDONLY) {
        rw_flags = Kernel::Process::FD_FLAG_READ;
    } else if (accmode == O_WRONLY) {
        rw_flags = Kernel::Process::FD_FLAG_WRITE;
    } else if (accmode == O_RDWR) {
        rw_flags = Kernel::Process::FD_FLAG_READ | Kernel::Process::FD_FLAG_WRITE;
    } else {
        return ERR_EINVAL;
    }

    uint32_t extra_flags = 0U;
    if ((flags & O_CLOEXEC) != 0ULL) {
        extra_flags |= Kernel::Process::FD_FLAG_CLOEXEC;
    }
    if ((flags & O_NONBLOCK) != 0ULL) {
        extra_flags |= Kernel::Process::FD_FLAG_NONBLOCK;
    }

    if (cstr_eq(canon_path, "/dev/console") || cstr_eq(canon_path, "/dev/tty")) {
        const int slot = Kernel::Process::allocate_fd(*proc);
        if (slot < 0) {
            return slot;
        }
        proc->fd_table[slot] = Kernel::Process::FileDescriptor{
            .type = Kernel::Process::FdType::ConsoleTty,
            .flags = rw_flags | extra_flags,
            .offset = 0U,
            .in_use = true,
        };
        return slot;
    }

    if (cstr_eq(canon_path, "/dev/null")) {
        const int slot = Kernel::Process::allocate_fd(*proc);
        if (slot < 0) {
            return slot;
        }
        proc->fd_table[slot] = Kernel::Process::FileDescriptor{
            .type = Kernel::Process::FdType::DevNull,
            .flags = rw_flags | extra_flags,
            .offset = 0U,
            .in_use = true,
        };
        return slot;
    }

    if ((flags & (O_CREAT | O_TRUNC)) != 0ULL || accmode != O_RDONLY) {
        return ERR_EROFS;
    }

    CpioEntry entry{};
    if (!resolve_initrd_path(canon_path, entry)) {
        return ERR_ENOENT;
    }

    if ((flags & O_DIRECTORY) != 0ULL && !entry.is_directory) {
        return ERR_ENOTDIR;
    }

    const int slot = Kernel::Process::allocate_fd(*proc);
    if (slot < 0) {
        return slot;
    }

    auto& desc = proc->fd_table[slot];
    desc = Kernel::Process::FileDescriptor{
        .type = Kernel::Process::FdType::InitrdFile,
        .flags = Kernel::Process::FD_FLAG_READ | extra_flags,
        .offset = 0U,
        .in_use = true,
        .cpio_entry = entry,
    };
    cstr_copy(desc.path, canon_path, sizeof(desc.path));
    return slot;
}

int64_t sys_close(uint64_t raw_fd) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || proc->fd_table == nullptr || raw_fd >= proc->fd_capacity) {
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
    if (proc == nullptr || proc->fd_table == nullptr || raw_fd >= proc->fd_capacity) {
        return ERR_EBADF;
    }

    auto& desc = proc->fd_table[static_cast<size_t>(raw_fd)];
    if (!desc.in_use) {
        return ERR_EBADF;
    }
    if (desc.type == Kernel::Process::FdType::DevNull) {
        return 0;
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
    if (proc == nullptr || proc->fd_table == nullptr || raw_fd >= proc->fd_capacity) {
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
        if (dirfd < 0 || proc->fd_table == nullptr ||
            static_cast<size_t>(dirfd) >= proc->fd_capacity) {
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

    if (cstr_eq(canon_path, "/dev/null")) {
        LinuxStat stat{};
        stat.st_dev = 1ULL;
        stat.st_ino = 4ULL;
        stat.st_mode = 0020666U;
        stat.st_nlink = 1U;
        stat.st_rdev = 0x0103ULL;
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

int64_t sys_faccessat2(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                       uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EACCESS | AT_EMPTY_PATH)) != 0ULL) {
        return ERR_EINVAL;
    }
    if ((mode & ~7ULL) != 0ULL) {
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
        if (dirfd < 0 || proc->fd_table == nullptr ||
            static_cast<size_t>(dirfd) >= proc->fd_capacity) {
            return ERR_EBADF;
        }
        const auto& desc = proc->fd_table[static_cast<size_t>(dirfd)];
        if (!desc.in_use) {
            return ERR_EBADF;
        }
        if (desc.type == Kernel::Process::FdType::InitrdFile && (mode & 2ULL) != 0ULL) {
            return ERR_EROFS;
        }
        return 0;
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

    if (cstr_eq(canon_path, "/dev/console") || cstr_eq(canon_path, "/dev/tty") ||
        cstr_eq(canon_path, "/dev/null")) {
        return ((mode & 1ULL) != 0ULL) ? ERR_EACCES : 0;
    }

    CpioEntry entry{};
    if (!resolve_initrd_path(canon_path, entry)) {
        return ERR_ENOENT;
    }

    if ((mode & 2ULL) != 0ULL) {
        return ERR_EROFS;
    }
    if ((mode & 1ULL) != 0ULL && !entry.is_directory && (entry.mode & 0111U) == 0U) {
        return ERR_EACCES;
    }
    return 0;
}

int64_t sys_faccessat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                      uint64_t flags) noexcept {
    return sys_faccessat2(dirfd, user_pathname, mode, flags);
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
        const char* exe_target = (proc->exe_path[0] != '\0') ? proc->exe_path : "/bin/bash";
        const size_t target_len = cstr_len(exe_target);
        const size_t copy_len = (bufsiz < target_len) ? static_cast<size_t>(bufsiz) : target_len;
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
    if (proc == nullptr || proc->fd_table == nullptr || raw_fd >= proc->fd_capacity) {
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
    if (proc == nullptr || proc->fd_table == nullptr || raw_oldfd >= proc->fd_capacity) {
        return ERR_EBADF;
    }
    const auto old_idx = static_cast<size_t>(raw_oldfd);
    if (!proc->fd_table[old_idx].in_use) {
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
    if (!proc->fd_table[old_idx].in_use) {
        return ERR_EBADF;
    }

    if (new_idx >= proc->fd_capacity) {
        if (!Kernel::Process::ensure_fd_capacity(*proc, new_idx + 1U)) {
            return ERR_ENOMEM;
        }
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
    if (proc == nullptr || proc->fd_table == nullptr || raw_fd >= proc->fd_capacity) {
        return ERR_EBADF;
    }
    const auto fd_idx = static_cast<size_t>(raw_fd);
    if (!proc->fd_table[fd_idx].in_use) {
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
        // Re-index proc->fd_table[fd_idx] after allocate_fd in case krealloc relocated the table
        Kernel::Process::copy_fd_slot(proc->fd_table[new_idx], proc->fd_table[fd_idx], true);
        if (cmd == F_DUPFD_CLOEXEC) {
            proc->fd_table[new_idx].flags |= Kernel::Process::FD_FLAG_CLOEXEC;
        }
        return new_idx;
    }

    case F_GETFD:
        return ((proc->fd_table[fd_idx].flags & Kernel::Process::FD_FLAG_CLOEXEC) != 0U)
                   ? static_cast<int64_t>(FD_CLOEXEC_BIT)
                   : 0;

    case F_SETFD:
        if ((arg & FD_CLOEXEC_BIT) != 0ULL) {
            proc->fd_table[fd_idx].flags |= Kernel::Process::FD_FLAG_CLOEXEC;
        } else {
            proc->fd_table[fd_idx].flags &= ~Kernel::Process::FD_FLAG_CLOEXEC;
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
        if ((desc.flags & Kernel::Process::FD_FLAG_NONBLOCK) != 0U) {
            flags |= O_NONBLOCK;
        }
        return static_cast<int64_t>(flags);
    }

    case F_SETFL:
        if ((arg & O_NONBLOCK) != 0ULL) {
            proc->fd_table[fd_idx].flags |= Kernel::Process::FD_FLAG_NONBLOCK;
        } else {
            proc->fd_table[fd_idx].flags &= ~Kernel::Process::FD_FLAG_NONBLOCK;
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
    proc->fd_table[read_fd].in_use = true;

    const int write_fd = Kernel::Process::allocate_fd(*proc, static_cast<size_t>(read_fd) + 1U);
    if (write_fd < 0) {
        proc->fd_table[read_fd].in_use = false;
        return write_fd;
    }

    Kernel::FS::Pipe* const pipe = Kernel::FS::allocate_pipe();
    if (pipe == nullptr) {
        proc->fd_table[read_fd].in_use = false;
        return ERR_ENOMEM;
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
// TTY, Multiplexing & I/O Syscalls
// ==========================================================================

int64_t sys_ioctl(uint64_t raw_fd, uint64_t request, uintptr_t arg) noexcept {
    const auto fd_idx = static_cast<size_t>(raw_fd);
    auto* const proc = Kernel::Process::current_process();
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        if (proc->fd_table == nullptr || fd_idx >= proc->fd_capacity) {
            return ERR_EBADF;
        }
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
        auto& cpu = HAL::get_cpu();
        const bool irq_was_enabled = cpu.interrupts_enabled();
        cpu.disable_interrupts();
        g_console_termios = updated;
        if (request == IOCTL_TCSETSF) {
            tty_flush_rx_unlocked();
        }
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
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
    const auto fd_idx = static_cast<size_t>(raw_fd);
    auto* const proc = Kernel::Process::current_process();
    uintptr_t root_paddr = 0U;
    bool nonblock = false;

    if (raw_count > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }
    const auto count = static_cast<size_t>(raw_count);

    if (proc != nullptr) {
        if (proc->fd_table == nullptr || fd_idx >= proc->fd_capacity) {
            return ERR_EBADF;
        }
        auto& desc = proc->fd_table[fd_idx];
        if (!desc.in_use || !desc.is_readable()) {
            return ERR_EBADF;
        }
        root_paddr = proc->address_space.root_paddr();
        nonblock = (desc.flags & Kernel::Process::FD_FLAG_NONBLOCK) != 0U;

        if (!validate_user_write_range(root_paddr, user_buf, count)) {
            return ERR_EFAULT;
        }
        if (count == 0U) {
            return 0;
        }

        if (desc.type == Kernel::Process::FdType::DevNull) {
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
    const bool echok = (g_console_termios.c_lflag & TERMIOS_ECHOK) != 0U;
    const bool map_cr_to_nl = (g_console_termios.c_iflag & TERMIOS_ICRNL) != 0U;
    const bool map_nl_to_cr = (g_console_termios.c_iflag & TERMIOS_INLCR) != 0U;
    const bool ignore_cr = (g_console_termios.c_iflag & TERMIOS_IGNCR) != 0U;

    auto translate_input_char = [&](char chr, bool& skip_char) -> char {
        skip_char = false;
        if (chr == '\r') {
            if (ignore_cr) {
                skip_char = true;
                return chr;
            }
            if (map_cr_to_nl) {
                return '\n';
            }
        } else if (chr == '\n' && map_nl_to_cr) {
            return '\r';
        }
        return chr;
    };

    char kbuf[READ_CHUNK_SIZE]{};
    size_t total_read = 0U;
    size_t chunk_pos = 0U;

    auto flush_chunk = [&]() -> int64_t {
        if (chunk_pos == 0U) {
            return 0;
        }
        const int64_t copy_res =
            Kernel::MM::copy_to_user(root_paddr, user_buf + total_read, kbuf, chunk_pos);
        if (copy_res < 0) {
            return copy_res;
        }
        total_read += chunk_pos;
        chunk_pos = 0U;
        return 0;
    };

    // Non-canonical mode (~TERMIOS_ICANON): POSIX VMIN / VTIME read semantics
    if (!canonical) {
        const uint8_t vmin = g_console_termios.c_cc[CC_VMIN];
        const uint8_t vtime = g_console_termios.c_cc[CC_VTIME];
        const size_t target_min = (vmin < count) ? vmin : count;
        const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
        const uint64_t timeout_ticks =
            (vtime > 0U && freq > 0ULL) ? ((static_cast<uint64_t>(vtime) * freq) / 10ULL) : 0ULL;
        uint64_t deadline_ticks =
            (timeout_ticks > 0ULL) ? (HAL::QemuVirt::GenericTimer::counter_ticks() + timeout_ticks)
                                   : 0ULL;

        while ((total_read + chunk_pos) < count) {
            uint8_t raw_byte = 0U;
            while ((total_read + chunk_pos) < count && tty_try_pop_byte(raw_byte)) {
                bool skip_char = false;
                const char chr = translate_input_char(static_cast<char>(raw_byte), skip_char);
                if (skip_char) {
                    continue;
                }
                kbuf[chunk_pos++] = chr;
                if (echo) {
                    HAL::get_console().putc(chr);
                }
                if (chunk_pos == READ_CHUNK_SIZE) {
                    const int64_t fres = flush_chunk();
                    if (fres < 0) {
                        return (total_read > 0U) ? static_cast<int64_t>(total_read) : fres;
                    }
                }
                if (timeout_ticks > 0ULL) {
                    deadline_ticks = HAL::QemuVirt::GenericTimer::counter_ticks() + timeout_ticks;
                }
            }

            const size_t buffered = total_read + chunk_pos;
            if (nonblock) {
                if (buffered > 0U) {
                    break;
                }
                if (proc != nullptr && Kernel::Process::has_unblocked_pending_signal(*proc)) {
                    return ERR_EINTR;
                }
                return ERR_EAGAIN;
            }

            if (vmin == 0U && vtime == 0U) {
                // Polling read: return immediately with whatever bytes are available
                break;
            }

            if (vmin > 0U && buffered >= target_min) {
                // Satisfied minimum byte threshold
                break;
            }

            if (vmin == 0U && vtime > 0U) {
                // Timed read: return as soon as >= 1 byte arrives or overall timer expires
                if (buffered > 0U) {
                    break;
                }
                if (HAL::QemuVirt::GenericTimer::counter_ticks() >= deadline_ticks) {
                    break;
                }
            } else if (vmin > 0U && vtime > 0U && buffered > 0U) {
                // Inter-byte timer active once at least 1 byte has been received
                if (HAL::QemuVirt::GenericTimer::counter_ticks() >= deadline_ticks) {
                    break;
                }
            }

            if (proc != nullptr && Kernel::Process::has_unblocked_pending_signal(*proc)) {
                const int64_t fres = flush_chunk();
                if (fres < 0 && total_read == 0U) {
                    return fres;
                }
                return (total_read > 0U) ? static_cast<int64_t>(total_read) : ERR_EINTR;
            }

            wait_for_event_or_tick();
        }

        const int64_t fres = flush_chunk();
        if (fres < 0 && total_read == 0U) {
            return fres;
        }
        return static_cast<int64_t>(total_read);
    }

    // Canonical mode (TERMIOS_ICANON): line-buffered input with erase/kill/EOF
    bool line_done = false;
    while ((total_read + chunk_pos) < count && !line_done) {
        uint8_t raw_byte = 0U;
        if (!tty_try_pop_byte(raw_byte)) {
            if (proc != nullptr && Kernel::Process::has_unblocked_pending_signal(*proc)) {
                const int64_t fres = flush_chunk();
                if (fres < 0 && total_read == 0U) {
                    return fres;
                }
                return (total_read > 0U) ? static_cast<int64_t>(total_read) : ERR_EINTR;
            }
            if (nonblock) {
                return ERR_EAGAIN;
            }
            wait_for_event_or_tick();
            continue;
        }

        if (raw_byte == g_console_termios.c_cc[CC_VEOF]) {
            break;
        }

        if (raw_byte == g_console_termios.c_cc[CC_VKILL]) {
            while (chunk_pos > 0U) {
                chunk_pos--;
                if (echo && echok) {
                    HAL::get_console().write("\b \b", 3U);
                }
            }
            continue;
        }

        if (raw_byte == '\b' || raw_byte == 0x7FU ||
            raw_byte == g_console_termios.c_cc[CC_VERASE]) {
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

        bool skip_char = false;
        const char chr = translate_input_char(static_cast<char>(raw_byte), skip_char);
        if (skip_char) {
            continue;
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

        if (chunk_pos == READ_CHUNK_SIZE || (total_read + chunk_pos) == count || line_done) {
            const int64_t fres = flush_chunk();
            if (fres < 0) {
                return (total_read > 0U) ? static_cast<int64_t>(total_read) : fres;
            }
        }
    }

    const int64_t fres = flush_chunk();
    if (fres < 0 && total_read == 0U) {
        return fres;
    }
    return static_cast<int64_t>(total_read);
}

int64_t sys_write(uint64_t raw_fd, uintptr_t user_buf, uint64_t raw_count) noexcept {
    const auto fd_idx = static_cast<size_t>(raw_fd);
    auto* const proc = Kernel::Process::current_process();
    uintptr_t root_paddr = 0U;

    if (raw_count > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }
    const auto count = static_cast<size_t>(raw_count);

    if (proc != nullptr) {
        if (proc->fd_table == nullptr || fd_idx >= proc->fd_capacity) {
            return ERR_EBADF;
        }
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

        if (desc.type == Kernel::Process::FdType::DevNull) {
            return static_cast<int64_t>(count);
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
    const auto fd_idx = static_cast<size_t>(raw_fd);
    auto* const proc = Kernel::Process::current_process();
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        if (proc->fd_table == nullptr || fd_idx >= proc->fd_capacity) {
            return ERR_EBADF;
        }
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

int16_t poll_fd_revents(const Kernel::Process::Process& proc, int32_t raw_fd,
                        int16_t events) noexcept {
    if (raw_fd < 0) {
        return 0;
    }
    if (proc.fd_table == nullptr || static_cast<size_t>(raw_fd) >= proc.fd_capacity) {
        return POLLNVAL;
    }
    const auto& desc = proc.fd_table[static_cast<size_t>(raw_fd)];
    if (!desc.in_use) {
        return POLLNVAL;
    }

    const auto event_mask = static_cast<uint16_t>(events);
    uint32_t revents = 0U;
    switch (desc.type) {
    case Kernel::Process::FdType::ConsoleTty:
        if (desc.is_readable() && tty_rx_available() > 0U) {
            revents |= event_mask & static_cast<uint16_t>(POLLIN | POLLRDNORM);
        }
        if (desc.is_writable()) {
            revents |= event_mask & static_cast<uint16_t>(POLLOUT | POLLWRNORM);
        }
        break;

    case Kernel::Process::FdType::DevNull:
    case Kernel::Process::FdType::InitrdFile:
        if (desc.is_readable()) {
            revents |= event_mask & static_cast<uint16_t>(POLLIN | POLLRDNORM);
        }
        if (desc.is_writable()) {
            revents |= event_mask & static_cast<uint16_t>(POLLOUT | POLLWRNORM);
        }
        break;

    case Kernel::Process::FdType::PipeRead:
        if (desc.pipe == nullptr || !desc.pipe->in_use) {
            revents |= static_cast<uint16_t>(POLLNVAL);
        } else {
            if (desc.pipe->count > 0U) {
                revents |= event_mask & static_cast<uint16_t>(POLLIN | POLLRDNORM);
            }
            if (desc.pipe->writers == 0U) {
                revents |= static_cast<uint16_t>(POLLHUP);
            }
        }
        break;

    case Kernel::Process::FdType::PipeWrite:
        if (desc.pipe == nullptr || !desc.pipe->in_use) {
            revents |= static_cast<uint16_t>(POLLNVAL);
        } else if (desc.pipe->readers == 0U) {
            revents |= static_cast<uint16_t>(POLLERR);
        } else if (desc.pipe->count < Kernel::FS::PIPE_BUF_SIZE) {
            revents |= event_mask & static_cast<uint16_t>(POLLOUT | POLLWRNORM);
        }
        break;

    default:
        revents |= static_cast<uint16_t>(POLLNVAL);
        break;
    }

    return static_cast<int16_t>(revents);
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
                !proc->fd_table[fd].in_use) {
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

    while (true) {
        uint64_t out_rfds[MAX_FDSET_WORDS]{};
        uint64_t out_wfds[MAX_FDSET_WORDS]{};
        uint64_t out_efds[MAX_FDSET_WORDS]{};
        int64_t ready_bits = 0;

        for (size_t fd = 0U; fd < u_nfds; ++fd) {
            const size_t word = fd / 64U;
            const uint64_t bit = 1ULL << (fd % 64U);

            if ((rfds[word] & bit) != 0ULL) {
                const int16_t rev =
                    poll_fd_revents(*proc, static_cast<int32_t>(fd), POLLIN | POLLRDNORM);
                if ((rev & (POLLIN | POLLRDNORM | POLLHUP | POLLERR)) != 0) {
                    out_rfds[word] |= bit;
                    ready_bits++;
                }
            }
            if ((wfds[word] & bit) != 0ULL) {
                const int16_t rev =
                    poll_fd_revents(*proc, static_cast<int32_t>(fd), POLLOUT | POLLWRNORM);
                if ((rev & (POLLOUT | POLLWRNORM | POLLERR)) != 0) {
                    out_wfds[word] |= bit;
                    ready_bits++;
                }
            }
            if ((efds[word] & bit) != 0ULL) {
                const int16_t rev = poll_fd_revents(*proc, static_cast<int32_t>(fd), 0);
                if ((rev & POLLERR) != 0) {
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
        unmap_and_free_range(proc->address_space, map_start, map_end, page_size);
    } else if (addr != 0U && (addr & page_mask) == 0U && addr < stack_floor &&
               length <= (stack_floor - addr) && addr >= MMAP_ARENA_FLOOR) {
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

    CpioEntry entry{};
    if (!resolve_initrd_path(canon_path, entry)) {
        free_frame(scratch_paddr);
        return ERR_ENOENT;
    }
    if (!entry.is_regular_file || entry.data == nullptr || entry.size == 0U) {
        free_frame(scratch_paddr);
        return ERR_ENOEXEC;
    }

    // Handle '#!' shebang scripts (e.g., #!/bin/bash or #!/bin/sh)
    char shebang_interp[Kernel::Process::MAX_PATH_LEN]{};
    char shebang_arg[Kernel::Process::MAX_PATH_LEN]{};
    bool has_shebang_arg = false;
    char exec_binary_path[Kernel::Process::MAX_PATH_LEN]{};
    cstr_copy(exec_binary_path, canon_path, sizeof(exec_binary_path));

    if (entry.size >= 2U && entry.data[0] == '#' && entry.data[1] == '!') {
        size_t pos = 2U;
        while (pos < entry.size && (entry.data[pos] == ' ' || entry.data[pos] == '\t')) {
            pos++;
        }
        size_t interp_len = 0U;
        while (pos < entry.size && entry.data[pos] != ' ' && entry.data[pos] != '\t' &&
               entry.data[pos] != '\n' && entry.data[pos] != '\r') {
            if (interp_len + 1U >= sizeof(shebang_interp)) {
                free_frame(scratch_paddr);
                return ERR_ENOEXEC;
            }
            shebang_interp[interp_len++] = static_cast<char>(entry.data[pos++]);
        }
        shebang_interp[interp_len] = '\0';
        if (interp_len == 0U) {
            free_frame(scratch_paddr);
            return ERR_ENOEXEC;
        }

        while (pos < entry.size && (entry.data[pos] == ' ' || entry.data[pos] == '\t')) {
            pos++;
        }
        size_t arg_len = 0U;
        while (pos < entry.size && entry.data[pos] != '\n' && entry.data[pos] != '\r') {
            if (arg_len + 1U < sizeof(shebang_arg)) {
                shebang_arg[arg_len++] = static_cast<char>(entry.data[pos]);
            }
            pos++;
        }
        while (arg_len > 0U &&
               (shebang_arg[arg_len - 1U] == ' ' || shebang_arg[arg_len - 1U] == '\t')) {
            arg_len--;
        }
        shebang_arg[arg_len] = '\0';
        has_shebang_arg = (arg_len > 0U);

        if (!normalize_vfs_path("/", shebang_interp, exec_binary_path, sizeof(exec_binary_path))) {
            free_frame(scratch_paddr);
            return ERR_ENOENT;
        }
        if (!resolve_initrd_path(exec_binary_path, entry) || !entry.is_regular_file ||
            entry.data == nullptr || entry.size == 0U) {
            free_frame(scratch_paddr);
            return ERR_ENOENT;
        }

        const int extra = has_shebang_arg ? 2 : 1;
        if (static_cast<size_t>(argc) + static_cast<size_t>(extra) > EXEC_MAX_ARGS) {
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

    if (proc->fd_table == nullptr &&
        !Kernel::Process::init_fd_table(*proc, Kernel::Process::INITIAL_FD_CAPACITY)) {
        free_frames(new_stack.phys_base, new_stack.num_pages);
        loaded.address_space.destroy();
        free_frame(scratch_paddr);
        return ERR_ENOMEM;
    }

    loaded.address_space.activate();

    if (proc->user_stack.phys_base != 0U && proc->user_stack.num_pages > 0U) {
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

void poll_console_tty() noexcept {
    if (!has_runnable_processes()) {
        return;
    }
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    tty_pump_uart_unlocked();

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void set_foreground_pgid(uint32_t pgid) noexcept {
    if (pgid > 0U) {
        g_foreground_pgid = pgid;
    }
}

uint32_t get_foreground_pgid() noexcept {
    return g_foreground_pgid;
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