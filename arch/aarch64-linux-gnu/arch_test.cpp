#include "../arch_test.hpp"

#include <cstddef>
#include <cstdint>

extern "C" {
void user_space_code();
int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp);
void trigger_svc_test();
void el0_sysreg_test_entry() noexcept;
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

} // namespace HAL::Test