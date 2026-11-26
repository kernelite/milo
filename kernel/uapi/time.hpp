#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef HOST_TEST
#undef RUSAGE_SELF
#undef RUSAGE_CHILDREN
#undef RUSAGE_THREAD
#endif

namespace Kernel::UAPI {

inline constexpr int32_t RUSAGE_SELF = 0;
inline constexpr int32_t RUSAGE_CHILDREN = -1;
inline constexpr int32_t RUSAGE_THREAD = 1;
inline constexpr size_t LINUX_RUSAGE_SIZE = 144U;

} // namespace Kernel::UAPI

extern "C" struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
static_assert(std::is_standard_layout_v<linux_timespec>, "linux_timespec must be standard layout");
static_assert(std::is_trivial_v<linux_timespec>, "linux_timespec must be trivial");
static_assert(sizeof(linux_timespec) == 16U, "linux_timespec must be 16 bytes");
static_assert(offsetof(linux_timespec, tv_sec) == 0U);
static_assert(offsetof(linux_timespec, tv_nsec) == 8U);

extern "C" struct linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};
static_assert(std::is_standard_layout_v<linux_timeval>, "linux_timeval must be standard layout");
static_assert(std::is_trivial_v<linux_timeval>, "linux_timeval must be trivial");
static_assert(sizeof(linux_timeval) == 16U, "linux_timeval must be 16 bytes");
static_assert(offsetof(linux_timeval, tv_sec) == 0U);
static_assert(offsetof(linux_timeval, tv_usec) == 8U);

extern "C" struct linux_tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};
static_assert(std::is_standard_layout_v<linux_tms>, "linux_tms must be standard layout");
static_assert(std::is_trivial_v<linux_tms>, "linux_tms must be trivial");
static_assert(sizeof(linux_tms) == 32U, "linux_tms must match Linux ABI (32 bytes)");
static_assert(offsetof(linux_tms, tms_utime) == 0U);
static_assert(offsetof(linux_tms, tms_stime) == 8U);
static_assert(offsetof(linux_tms, tms_cutime) == 16U);
static_assert(offsetof(linux_tms, tms_cstime) == 24U);

extern "C" struct linux_rusage {
    linux_timeval ru_utime;
    linux_timeval ru_stime;
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
static_assert(std::is_standard_layout_v<linux_rusage>, "linux_rusage must be standard layout");
static_assert(std::is_trivial_v<linux_rusage>, "linux_rusage must be trivial");
static_assert(sizeof(linux_rusage) == Kernel::UAPI::LINUX_RUSAGE_SIZE,
              "linux_rusage must match Linux AArch64 ABI (144 bytes)");
static_assert(offsetof(linux_rusage, ru_utime) == 0U);
static_assert(offsetof(linux_rusage, ru_stime) == 16U);
static_assert(offsetof(linux_rusage, ru_maxrss) == 32U);
static_assert(offsetof(linux_rusage, ru_minflt) == 64U);
static_assert(offsetof(linux_rusage, ru_nvcsw) == 128U);
static_assert(offsetof(linux_rusage, ru_nivcsw) == 136U);
