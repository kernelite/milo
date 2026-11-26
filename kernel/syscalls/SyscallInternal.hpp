#pragma once

#include "fs/Vfs.hpp"
#include "hal/mmu.hpp"
#include "mm/AddressSpace.hpp"
#include "process/Process.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Syscalls {

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

// Shared syscall helper functions
void wait_for_event_or_tick() noexcept;
[[nodiscard]] uint64_t timespec_to_timer_ticks(const LinuxTimespec& time_spec,
                                               uint64_t freq) noexcept;
[[nodiscard]] uint64_t monotonic_now_ns() noexcept;

[[nodiscard]] size_t cstr_len(const char* str) noexcept;
void cstr_copy(char* dst, const char* src, size_t cap) noexcept;
[[nodiscard]] bool cstr_eq(const char* lhs, const char* rhs) noexcept;

[[nodiscard]] bool normalize_vfs_path(const char* base_dir, const char* input, char* out,
                                      size_t out_cap) noexcept;
[[nodiscard]] Kernel::FS::File* get_proc_file(Kernel::Process::Process* proc,
                                              uint64_t raw_fd) noexcept;
void fill_stat_from_inode(const Kernel::FS::Inode& inode, LinuxStat& stat_out) noexcept;

void unmap_and_free_range(Kernel::MM::AddressSpace& adrsp, uintptr_t start_vaddr,
                          uintptr_t end_vaddr, size_t page_size) noexcept;
[[nodiscard]] bool validate_user_range(uintptr_t root_paddr, uintptr_t user_buf, size_t count,
                                       HAL::PageFlags required_perm) noexcept;
[[nodiscard]] bool validate_user_read_range(uintptr_t root_paddr, uintptr_t user_buf,
                                            size_t count) noexcept;
[[nodiscard]] bool validate_user_write_range(uintptr_t root_paddr, uintptr_t user_buf,
                                             size_t count) noexcept;

[[nodiscard]] int64_t copy_user_cstring(uintptr_t root_paddr, uintptr_t user_str, char* dst,
                                        size_t max_len, size_t& out_len) noexcept;
[[nodiscard]] int64_t stage_user_string_array(uintptr_t root_paddr, uintptr_t user_array,
                                              const char** out_ptrs, size_t max_entries, char* pool,
                                              size_t pool_cap, size_t& pool_offset,
                                              int& out_count) noexcept;

[[nodiscard]] int64_t resolve_dirfd_dentry(Kernel::Process::Process& proc, int32_t dirfd,
                                           Kernel::FS::Dentry*& out_base) noexcept;
void build_dentry_path(const Kernel::FS::Dentry* target, char* out, size_t cap) noexcept;

// Memory management syscalls (MemorySyscalls.cpp)
[[nodiscard]] int64_t sys_brk(uintptr_t addr) noexcept;
[[nodiscard]] int64_t sys_mmap(uintptr_t addr, uint64_t length, uint64_t prot, uint64_t flags,
                               uint64_t raw_fd, uint64_t offset) noexcept;
[[nodiscard]] int64_t sys_munmap(uintptr_t addr, uint64_t length) noexcept;
[[nodiscard]] int64_t sys_mprotect(uintptr_t addr, uint64_t length, uint64_t prot) noexcept;

// Time & system information syscalls (TimeSyscalls.cpp)
[[nodiscard]] int64_t sys_clock_gettime(uint64_t clk_id, uintptr_t user_tp) noexcept;
[[nodiscard]] int64_t sys_nanosleep(uintptr_t user_req, uintptr_t user_rem) noexcept;
[[nodiscard]] int64_t sys_times(uintptr_t user_buf) noexcept;
[[nodiscard]] int64_t sys_getrusage(int32_t who, uintptr_t user_usage) noexcept;
[[nodiscard]] int64_t sys_sysinfo(uintptr_t user_info) noexcept;
[[nodiscard]] int64_t sys_getrandom(uintptr_t user_buf, uint64_t raw_buflen,
                                    uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_uname(uintptr_t user_buf) noexcept;

} // namespace Kernel::Syscalls
