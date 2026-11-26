#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef HOST_TEST
#undef CLONE_VM
#undef CLONE_FS
#undef CLONE_FILES
#undef CLONE_SIGHAND
#undef CLONE_VFORK
#undef CLONE_THREAD
#undef CLONE_PARENT_SETTID
#undef CLONE_CHILD_CLEARTID
#undef CLONE_CHILD_SETTID
#undef CSIGNAL_MASK
#undef PROT_NONE
#undef PROT_READ
#undef PROT_WRITE
#undef PROT_EXEC
#undef MAP_SHARED
#undef MAP_PRIVATE
#undef MAP_FIXED
#undef MAP_ANONYMOUS
#undef GRND_NONBLOCK
#undef GRND_RANDOM
#undef GRND_INSECURE
#undef POLLIN
#undef POLLOUT
#undef POLLERR
#undef POLLHUP
#undef POLLNVAL
#undef POLLRDNORM
#undef POLLWRNORM
#undef FD_SETSIZE
#endif

namespace Kernel::UAPI {

// Linux clone() flags
inline constexpr uint64_t CLONE_VM = 0x00000100ULL;
inline constexpr uint64_t CLONE_FS = 0x00000200ULL;
inline constexpr uint64_t CLONE_FILES = 0x00000400ULL;
inline constexpr uint64_t CLONE_SIGHAND = 0x00000800ULL;
inline constexpr uint64_t CLONE_VFORK = 0x00004000ULL;
inline constexpr uint64_t CLONE_THREAD = 0x00010000ULL;
inline constexpr uint64_t CLONE_PARENT_SETTID = 0x00100000ULL;
inline constexpr uint64_t CLONE_CHILD_CLEARTID = 0x00200000ULL;
inline constexpr uint64_t CLONE_CHILD_SETTID = 0x01000000ULL;
inline constexpr uint64_t CSIGNAL_MASK = 0x000000FFULL;

// Linux wait4() options
inline constexpr uint64_t WAIT_WNOHANG = 0x00000001ULL;
inline constexpr uint64_t WAIT_WUNTRACED = 0x00000002ULL;
inline constexpr uint64_t WAIT_WCONTINUED = 0x00000008ULL;

// mmap() / mprotect() protection & mapping flags
inline constexpr uint64_t PROT_NONE = 0x0ULL;
inline constexpr uint64_t PROT_READ = 0x1ULL;
inline constexpr uint64_t PROT_WRITE = 0x2ULL;
inline constexpr uint64_t PROT_EXEC = 0x4ULL;

inline constexpr uint64_t MAP_SHARED = 0x01ULL;
inline constexpr uint64_t MAP_PRIVATE = 0x02ULL;
inline constexpr uint64_t MAP_FIXED = 0x10ULL;
inline constexpr uint64_t MAP_ANONYMOUS = 0x20ULL;

// getrandom() flags
inline constexpr uint64_t GRND_NONBLOCK = 0x0001ULL;
inline constexpr uint64_t GRND_RANDOM = 0x0002ULL;
inline constexpr uint64_t GRND_INSECURE = 0x0004ULL;

// ppoll() / pselect6() event bitmasks & fd_set bounds
inline constexpr int16_t POLLIN = 0x0001;
inline constexpr int16_t POLLOUT = 0x0004;
inline constexpr int16_t POLLERR = 0x0008;
inline constexpr int16_t POLLHUP = 0x0010;
inline constexpr int16_t POLLNVAL = 0x0020;
inline constexpr int16_t POLLRDNORM = 0x0040;
inline constexpr int16_t POLLWRNORM = 0x0100;
inline constexpr size_t FD_SETSIZE = 1024U;

} // namespace Kernel::UAPI

extern "C" struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};
static_assert(std::is_standard_layout_v<linux_utsname>, "linux_utsname must be standard layout");
static_assert(std::is_trivial_v<linux_utsname>, "linux_utsname must be trivial");
static_assert(sizeof(linux_utsname) == 390U, "linux_utsname must be 390 bytes");
static_assert(offsetof(linux_utsname, sysname) == 0U);
static_assert(offsetof(linux_utsname, nodename) == 65U);
static_assert(offsetof(linux_utsname, release) == 130U);
static_assert(offsetof(linux_utsname, version) == 195U);
static_assert(offsetof(linux_utsname, machine) == 260U);
static_assert(offsetof(linux_utsname, domainname) == 325U);

extern "C" struct linux_sysinfo {
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
static_assert(std::is_standard_layout_v<linux_sysinfo>, "linux_sysinfo must be standard layout");
static_assert(std::is_trivial_v<linux_sysinfo>, "linux_sysinfo must be trivial");
static_assert(sizeof(linux_sysinfo) == 112U, "linux_sysinfo must be 112 bytes");
static_assert(offsetof(linux_sysinfo, uptime) == 0U);
static_assert(offsetof(linux_sysinfo, loads) == 8U);
static_assert(offsetof(linux_sysinfo, totalram) == 32U);
static_assert(offsetof(linux_sysinfo, freeram) == 40U);
static_assert(offsetof(linux_sysinfo, procs) == 80U);
static_assert(offsetof(linux_sysinfo, totalhigh) == 88U);
static_assert(offsetof(linux_sysinfo, mem_unit) == 104U);

extern "C" struct linux_rlimit64 {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};
static_assert(std::is_standard_layout_v<linux_rlimit64>, "linux_rlimit64 must be standard layout");
static_assert(std::is_trivial_v<linux_rlimit64>, "linux_rlimit64 must be trivial");
static_assert(sizeof(linux_rlimit64) == 16U, "linux_rlimit64 must be 16 bytes");
static_assert(offsetof(linux_rlimit64, rlim_cur) == 0U);
static_assert(offsetof(linux_rlimit64, rlim_max) == 8U);

extern "C" struct linux_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};
static_assert(std::is_standard_layout_v<linux_pollfd>, "linux_pollfd must be standard layout");
static_assert(std::is_trivial_v<linux_pollfd>, "linux_pollfd must be trivial");
static_assert(sizeof(linux_pollfd) == 8U, "linux_pollfd must be 8 bytes");
static_assert(offsetof(linux_pollfd, fd) == 0U);
static_assert(offsetof(linux_pollfd, events) == 4U);
static_assert(offsetof(linux_pollfd, revents) == 6U);
