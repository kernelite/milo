#include "process/process.hpp"

namespace Kernel::Process {

void init_user_cpu_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t sp_el0,
                           uint64_t argc, uintptr_t argv_vaddr) noexcept {
    // Zero out all general purpose registers (x0-x30) to prevent state leaks
    for (size_t i = 0U; i < 31U; ++i) {
        ctx.x[i] = 0ULL;
    }

    ctx.x[0] = argc;           // x0 = argc
    ctx.x[1] = argv_vaddr;     // x1 = Address of argv array
    ctx.sp_el0 = sp_el0;       // 16-byte aligned user stack pointer
    ctx.elr_el1 = entry_point; // Entry point (e_entry from ELF header)
    ctx.spsr_el1 = 0x00ULL;    // EL0t mode with DAIF exceptions unmasked
}

} // namespace Kernel::Process