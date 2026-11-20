#pragma once

#include <cstdint>

namespace Kernel::Process {

// Linux asm-generic/resource.h resource identifiers
constexpr uint32_t RLIMIT_CPU = 0U;
constexpr uint32_t RLIMIT_FSIZE = 1U;
constexpr uint32_t RLIMIT_DATA = 2U;
constexpr uint32_t RLIMIT_STACK = 3U;
constexpr uint32_t RLIMIT_CORE = 4U;
constexpr uint32_t RLIMIT_RSS = 5U;
constexpr uint32_t RLIMIT_NPROC = 6U;
constexpr uint32_t RLIMIT_NOFILE = 7U;
constexpr uint32_t RLIMIT_MEMLOCK = 8U;
constexpr uint32_t RLIMIT_AS = 9U;
constexpr uint32_t RLIM_NLIMITS = 16U;
constexpr uint64_t RLIM64_INFINITY = ~0ULL;

// Default POSIX open-file limits (matching standard Linux defaults)
constexpr uint64_t DEFAULT_RLIMIT_NOFILE_CUR = 1024ULL;
constexpr uint64_t DEFAULT_RLIMIT_NOFILE_MAX = 4096ULL;

// Dynamic process limit scaling baseline (1 process per MiB of usable RAM)
constexpr uint64_t MIN_RLIMIT_NPROC = 64ULL;
constexpr uint64_t BYTES_PER_PROCESS_QUOTA = 1024ULL * 1024ULL;

struct RLimit64 {
    uint64_t rlim_cur{RLIM64_INFINITY};
    uint64_t rlim_max{RLIM64_INFINITY};
};

void init_resource_limits() noexcept;
[[nodiscard]] uint64_t default_rlimit_nproc() noexcept;

} // namespace Kernel::Process