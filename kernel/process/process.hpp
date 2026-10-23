#pragma once

#include "hal/cpu.hpp"
#include "process/stack_builder.hpp"

#include <cstdint>

namespace Kernel::Process {

struct ProcessContext {
    HAL::CpuContext cpu_ctx{};
    UserStack user_stack{};
    uintptr_t entry_point{0U};
};

void init_user_cpu_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t sp_el0,
                           uint64_t argc, uintptr_t argv_vaddr) noexcept;

extern "C" int64_t enter_user_context(const HAL::CpuContext* ctx) noexcept;

} // namespace Kernel::Process