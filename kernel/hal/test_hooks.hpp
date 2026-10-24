#pragma once

#include <cstdint>

namespace HAL::Test {

// Read-Only (.rodata) fault test hooks
uintptr_t get_ro_target_address() noexcept;
void trigger_ro_fault() noexcept;

// Execute-Never (.data) fault test hooks
uintptr_t get_nx_target_address() noexcept;
void trigger_nx_fault() noexcept;

// User space / privilege fault hooks
void trigger_privileged_sysreg_fault() noexcept;
int64_t run_user_mode_test() noexcept;
void trigger_user_syscall_test() noexcept;

} // namespace HAL::Test