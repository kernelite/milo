#include "hal/test_hooks.hpp"

#include <cstddef>
#include <cstdint>

extern "C" {
void user_space_code();
int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp);
void trigger_svc_test();
void el0_sysreg_test_entry() noexcept;
void* memcpy(void* dest, const void* src, size_t count) noexcept;

extern const uint8_t el0_tty_ioctl_suite_start[];
extern const uint8_t el0_tty_ioctl_suite_end[];
extern const uint8_t el0_tty_echo_start[];
extern const uint8_t el0_tty_echo_end[];
extern const uint8_t el0_user_echo_start[];
extern const uint8_t el0_user_echo_end[];
extern const uint8_t el0_user_mm_suite_start[];
extern const uint8_t el0_user_mm_suite_end[];
extern const uint8_t el0_proc_lifecycle_suite_start[];
extern const uint8_t el0_proc_lifecycle_suite_end[];
extern const uint8_t el0_fork_exec_suite_start[];
extern const uint8_t el0_fork_exec_suite_end[];
extern const uint8_t el0_sched_suite_start[];
extern const uint8_t el0_sched_suite_end[];
}

namespace {

// Explicitly placed in .rodata segment (Read-Only)
__attribute__((section(".rodata"))) const uint32_t g_rodata_test_target = 0xDEADBEEFU;

// Explicitly placed in .data segment (Writable + Non-Executable)
// Contains ARM64 'ret' (0xD65F03C0) and 'nop' (0xD503201F) instructions
__attribute__((section(".data"))) const uint32_t g_data_nx_target[2] = {
    0xD65F03C0U, // ret
    0xD503201FU  // nop
};

alignas(16) uint8_t g_el0_test_stack[4096];
alignas(16) uint8_t g_user_test_stack[2048];
alignas(16) uint8_t g_user_svc_stack[4096];

} // namespace

namespace HAL::Test {

uintptr_t get_ro_target_address() noexcept {
    return reinterpret_cast<uintptr_t>(&g_rodata_test_target);
}

void trigger_ro_fault() noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto* const mutable_ptr = const_cast<volatile uint32_t*>(&g_rodata_test_target);
    // Triggers EL1 Data Abort; trap handler advances ELR_EL1 past this store
    *mutable_ptr = 0xCAFEBABE;
}

uintptr_t get_nx_target_address() noexcept {
    return reinterpret_cast<uintptr_t>(g_data_nx_target);
}

void trigger_nx_fault() noexcept {
    using FuncPtr = void (*)() noexcept;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto code_in_data = reinterpret_cast<FuncPtr>(const_cast<uint32_t*>(g_data_nx_target));
    // Triggers EL1 Instruction Abort; trap handler advances ELR_EL1 past instruction
    code_in_data();
}

void trigger_privileged_sysreg_fault() noexcept {
    const uintptr_t user_sp = reinterpret_cast<uintptr_t>(&g_el0_test_stack[4096]);
    const uintptr_t entry = reinterpret_cast<uintptr_t>(el0_sysreg_test_entry);
    enter_user_mode(entry, user_sp);
}

int64_t run_user_mode_test() noexcept {
    const uintptr_t stack_top =
        reinterpret_cast<uintptr_t>(g_user_test_stack) + sizeof(g_user_test_stack);
    return enter_user_mode(reinterpret_cast<uintptr_t>(user_space_code), stack_top);
}

void trigger_user_syscall_test() noexcept {
    const uint64_t user_sp =
        reinterpret_cast<uint64_t>(g_user_svc_stack) + sizeof(g_user_svc_stack);
    enter_user_mode(reinterpret_cast<uint64_t>(trigger_svc_test), user_sp);
    asm volatile("" ::: "memory"); // Prevent tail-call optimization
}

uintptr_t get_privileged_mmio_probe_address() noexcept {
    return 0x0000000009000000ULL; // QEMU virt PL011 UART MMIO (AP[1] == 0)
}

uintptr_t get_kernel_code_probe_address() noexcept {
    return 0x0000000040080000ULL; // Kernel .text base (>= user_va_limit)
}

size_t write_user_exit_program(void* dst_kva, int64_t exit_code) noexcept {
    auto* const out = static_cast<uint32_t*>(dst_kva);
    const auto imm16 = static_cast<uint32_t>(static_cast<uint64_t>(exit_code) & 0xFFFFU);
    out[0] = 0xD2800BA8U;                 // mov x8, #93 (SYS_EXIT)
    out[1] = 0xD2800000U | (imm16 << 5U); // mov x0, #exit_code
    out[2] = 0xD4000001U;                 // svc #0
    out[3] = 0xD65F03C0U;                 // ret
    return 4U * sizeof(uint32_t);
}

size_t write_user_mm_suite_program(void* dst_kva) noexcept {
    const auto size = static_cast<size_t>(el0_user_mm_suite_end - el0_user_mm_suite_start);
    memcpy(dst_kva, el0_user_mm_suite_start, size);
    return size;
}

size_t write_user_syscall_suite_program(void* dst_kva, uintptr_t valid_buf_vaddr,
                                        size_t valid_buf_len, uintptr_t crossing_buf_vaddr,
                                        size_t crossing_buf_len,
                                        uintptr_t kernel_buf_vaddr) noexcept {
    auto* const out = static_cast<uint32_t*>(dst_kva);
    size_t idx = 0U;

    auto emit_mov_imm64 = [&](uint32_t reg, uint64_t val) {
        out[idx++] = 0xD2800000U | (static_cast<uint32_t>(val & 0xFFFFU) << 5U) | reg;
        out[idx++] = 0xF2A00000U | (static_cast<uint32_t>((val >> 16U) & 0xFFFFU) << 5U) | reg;
        out[idx++] = 0xF2C00000U | (static_cast<uint32_t>((val >> 32U) & 0xFFFFU) << 5U) | reg;
        out[idx++] = 0xF2E00000U | (static_cast<uint32_t>((val >> 48U) & 0xFFFFU) << 5U) | reg;
    };

    // x19 = failure accumulator
    out[idx++] = 0xD2800013U; // mov x19, #0

    // 1) Valid SYS_WRITE(fd=1, valid_buf_vaddr, valid_buf_len) -> expect valid_buf_len
    emit_mov_imm64(8U, 64ULL);
    emit_mov_imm64(0U, 1ULL);
    emit_mov_imm64(1U, valid_buf_vaddr);
    emit_mov_imm64(2U, valid_buf_len);
    out[idx++] = 0xD4000001U; // svc #0
    emit_mov_imm64(3U, valid_buf_len);
    out[idx++] = 0xEB03001FU; // cmp x0, x3
    out[idx++] = 0x54000040U; // b.eq +8
    out[idx++] = 0x91000673U; // add x19, x19, #1

    // 2) Kernel pointer SYS_WRITE(fd=1, kernel_buf_vaddr, 4) -> expect -14 (-EFAULT)
    emit_mov_imm64(8U, 64ULL);
    emit_mov_imm64(0U, 1ULL);
    emit_mov_imm64(1U, kernel_buf_vaddr);
    emit_mov_imm64(2U, 4ULL);
    out[idx++] = 0xD4000001U; // svc #0
    out[idx++] = 0xB100381FU; // cmn x0, #14
    out[idx++] = 0x54000040U; // b.eq +8
    out[idx++] = 0x91000673U; // add x19, x19, #1

    // 3) Unmapped-crossing buffer SYS_WRITE(fd=1, crossing_buf_vaddr, crossing_buf_len) -> -14
    emit_mov_imm64(8U, 64ULL);
    emit_mov_imm64(0U, 1ULL);
    emit_mov_imm64(1U, crossing_buf_vaddr);
    emit_mov_imm64(2U, crossing_buf_len);
    out[idx++] = 0xD4000001U; // svc #0
    out[idx++] = 0xB100381FU; // cmn x0, #14
    out[idx++] = 0x54000040U; // b.eq +8
    out[idx++] = 0x91000673U; // add x19, x19, #1

    // 4) Invalid/closed FD SYS_WRITE(fd=9, valid_buf_vaddr, 4) -> expect -9 (-EBADF)
    emit_mov_imm64(8U, 64ULL);
    emit_mov_imm64(0U, 9ULL);
    emit_mov_imm64(1U, valid_buf_vaddr);
    emit_mov_imm64(2U, 4ULL);
    out[idx++] = 0xD4000001U; // svc #0
    out[idx++] = 0xB100241FU; // cmn x0, #9
    out[idx++] = 0x54000040U; // b.eq +8
    out[idx++] = 0x91000673U; // add x19, x19, #1

    // 5) Unimplemented syscall (999) -> expect -38 (-ENOSYS)
    emit_mov_imm64(8U, 999ULL);
    out[idx++] = 0xD4000001U; // svc #0
    out[idx++] = 0xB100981FU; // cmn x0, #38
    out[idx++] = 0x54000040U; // b.eq +8
    out[idx++] = 0x91000673U; // add x19, x19, #1

    // 6) SYS_EXIT(x19)
    emit_mov_imm64(8U, 93ULL);
    out[idx++] = 0xAA1303E0U; // mov x0, x19
    out[idx++] = 0xD4000001U; // svc #0

    return idx * sizeof(uint32_t);
}

size_t write_user_tty_ioctl_suite_program(void* dst_kva) noexcept {
    const auto size = static_cast<size_t>(el0_tty_ioctl_suite_end - el0_tty_ioctl_suite_start);
    memcpy(dst_kva, el0_tty_ioctl_suite_start, size);
    return size;
}

size_t write_user_tty_echo_program(void* dst_kva) noexcept {
    const auto size = static_cast<size_t>(el0_tty_echo_end - el0_tty_echo_start);
    memcpy(dst_kva, el0_tty_echo_start, size);
    return size;
}

size_t write_user_echo_program(void* dst_kva) noexcept {
    const auto size = static_cast<size_t>(el0_user_echo_end - el0_user_echo_start);
    memcpy(dst_kva, el0_user_echo_start, size);
    return size;
}

// Inside namespace HAL::Test in arch/aarch64-linux-gnu/arch_test.cpp:
size_t write_user_proc_lifecycle_program(void* dst_kva) noexcept {
    const auto size =
        static_cast<size_t>(el0_proc_lifecycle_suite_end - el0_proc_lifecycle_suite_start);
    memcpy(dst_kva, el0_proc_lifecycle_suite_start, size);
    return size;
}

size_t write_user_fork_exec_program(void* dst_kva) noexcept {
    const auto size = static_cast<size_t>(el0_fork_exec_suite_end - el0_fork_exec_suite_start);
    memcpy(dst_kva, el0_fork_exec_suite_start, size);
    return size;
}

size_t write_user_sched_suite_program(void* dst_kva) noexcept {
    const auto size = static_cast<size_t>(el0_sched_suite_end - el0_sched_suite_start);
    memcpy(dst_kva, el0_sched_suite_start, size);
    return size;
}
} // namespace HAL::Test