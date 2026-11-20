#include "process/resource.hpp"

#include "hal/cpu.hpp"

namespace {

uint64_t g_default_rlimit_nproc = 0ULL;

uint64_t compute_nproc_from_ram() noexcept {
    const size_t ram_size = HAL::get_platform_info().ram_size;
    uint64_t computed = static_cast<uint64_t>(ram_size) / Kernel::Process::BYTES_PER_PROCESS_QUOTA;
    if (computed < Kernel::Process::MIN_RLIMIT_NPROC) {
        computed = Kernel::Process::MIN_RLIMIT_NPROC;
    }
    return computed;
}

} // namespace

namespace Kernel::Process {

void init_resource_limits() noexcept {
    g_default_rlimit_nproc = compute_nproc_from_ram();
}

uint64_t default_rlimit_nproc() noexcept {
    if (g_default_rlimit_nproc == 0ULL) {
        g_default_rlimit_nproc = compute_nproc_from_ram();
    }
    return g_default_rlimit_nproc;
}

} // namespace Kernel::Process