#pragma once

#include <cstddef>
#include <cstdint>

namespace HAL::Test {

struct TtyIoctlSuiteArgs {
    uint64_t rw_buf_vaddr;
    uint64_t ro_buf_vaddr;
    uint64_t crossing_buf_vaddr;
    uint64_t crossing_buf_len;
    uint64_t non_tty_fd;
};

struct TtyEchoArgs {
    uint64_t prompt_vaddr;
    uint64_t prompt_len;
    uint64_t prefix_vaddr;
    uint64_t prefix_len;
    uint64_t line_buf_vaddr;
    uint64_t line_buf_cap;
};

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

// Architecture-specific EL0 test payload & address probes
uintptr_t get_privileged_mmio_probe_address() noexcept;
uintptr_t get_kernel_code_probe_address() noexcept;
size_t write_user_exit_program(void* dst_kva, int64_t exit_code) noexcept;
size_t write_user_syscall_suite_program(void* dst_kva, uintptr_t valid_buf_vaddr,
                                        size_t valid_buf_len, uintptr_t crossing_buf_vaddr,
                                        size_t crossing_buf_len,
                                        uintptr_t kernel_buf_vaddr) noexcept;
size_t write_user_tty_ioctl_suite_program(void* dst_kva) noexcept;
size_t write_user_tty_echo_program(void* dst_kva) noexcept;

} // namespace HAL::Test