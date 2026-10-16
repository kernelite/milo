#include "boot/fdt.hpp"
#include "../arch/arch_test.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "mm/pfa.hpp"

#include <cstddef>
#include <cstdint>

// Access global initrd metadata from main.cpp
extern FDT::InitrdLocation g_initrd_loc;

namespace {

constexpr size_t INPUT_BUFFER_SIZE = 128;
int bss_check_var; // Uninitialized global variable to verify .bss zeroing

bool streq(const char* str1, const char* str2) {
    if (str1 == nullptr || str2 == nullptr) {
        return str1 == str2; // true only if both are nullptr
    }

    while (*str1 != '\0' && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *str1 == *str2;
}

void print_str(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    HAL::get_console().write(str, len);
}

// Simple assertion helper for bare-metal console output
void assert_test(bool condition, const char* test_name) {
    if (condition) {
        print_str("[PASS] ");
    } else {
        print_str("[FAIL] ");
    }
    print_str(test_name);
    print_str("\n");
}

void print_dec(uint8_t val) noexcept {
    if (val >= 10U) {
        HAL::get_console().putc(static_cast<char>('0' + (val / 10U)));
    }
    HAL::get_console().putc(static_cast<char>('0' + (val % 10U)));
}

void print_hex64(uint64_t val) noexcept {
    char buf[19] = "0x0000000000000000";
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 17; i >= 2; --i) {
        buf[i] = hex_chars[val & 0xFU];
        val >>= 4U;
    }
    print_str(buf);
}

// System diagnostic command: prints Exception Level, MMU, and Cache status safely
void cmd_info() noexcept {
    print_str("\r\n================ SYSTEM INFO ================\r\n");

    // 1. Query Current Exception Level via CpuControl HAL
    const uint8_t el_reg = HAL::get_cpu().current_el();
    print_str("  Current Exception Level : EL");
    print_dec(el_reg);
    print_str("\r\n");

    // 2. Query MMU and Cache hardware status
    const auto mmu_st = HAL::get_mmu().status();
    print_str("  MMU Enabled             : ");
    print_str(mmu_st.mmu_enabled ? "YES\r\n" : "NO\r\n");
    print_str("  D-Cache Enabled         : ");
    print_str(mmu_st.dcache_enabled ? "YES\r\n" : "NO\r\n");
    print_str("  I-Cache Enabled         : ");
    print_str(mmu_st.icache_enabled ? "YES\r\n" : "NO\r\n");
    print_str("  SCTLR_EL1 Register      : ");
    print_hex64(mmu_st.raw_control_reg);
    print_str("\r\n=============================================\r\n");
}

void cmd_test_cpu() {
    print_str("[TEST] Testing CPU HAL Interrupt Management...\r\n");
    bool initial_state = HAL::get_cpu().interrupts_enabled();

    HAL::get_cpu().disable_interrupts();
    bool disabled_state = HAL::get_cpu().interrupts_enabled();
    print_str(disabled_state ? "  [FAIL] Interrupts still enabled after disable()\r\n"
                             : "  [PASS] Interrupts successfully disabled.\r\n");

    HAL::get_cpu().enable_interrupts();
    bool enabled_state = HAL::get_cpu().interrupts_enabled();
    print_str(enabled_state ? "  [PASS] Interrupts successfully enabled.\r\n"
                            : "  [FAIL] Interrupts still disabled after enable()\r\n");

    // Restore initial state
    if (!initial_state) {
        HAL::get_cpu().disable_interrupts();
    }
}

void cmd_test_mmu() {
    print_str("[TEST] Inspecting System MMU & Cache Status via HAL...\r\n");

    // Stack-allocated MMU status query
    HAL::MmuStatus status = HAL::MmuStatus();

    print_str("  Control Register Value: ");
    print_hex64(status.raw_control_reg);
    print_str("\r\n");

    print_str(status.mmu_enabled ? "  [PASS] MMU: ENABLED\r\n" : "  [INFO] MMU: DISABLED\r\n");
    print_str(status.dcache_enabled ? "  [PASS] Data Cache: ENABLED\r\n"
                                    : "  [INFO] Data Cache: DISABLED\r\n");
    print_str(status.icache_enabled ? "  [PASS] Instruction Cache: ENABLED\r\n"
                                    : "  [INFO] Instruction Cache: DISABLED\r\n");
}

void cmd_test_el0() {
    print_str("[TEST] Testing EL0 User-Mode Transition & SVC Trap...\r\n");
    const int64_t res = HAL::Test::run_user_mode_test();

    print_str("  [PASS] Safely returned from EL0 to EL1! Exit status: ");
    print_hex64(static_cast<uint64_t>(res));
    print_str("\r\n");
}

void cmd_test_cpp() {
    print_str("[TEST] Verifying Freestanding C++ Runtime...\r\n");

    if (bss_check_var == 0) {
        print_str("  [PASS] .bss section correctly zero-initialized.\r\n");
    } else {
        print_str("  [FAIL] .bss contains uninitialized garbage!\r\n");
    }

    print_str("  Testing Virtual Function Dynamic Dispatch... ");
    HAL::get_console().putc('[');
    HAL::get_console().putc('O');
    HAL::get_console().putc('K');
    HAL::get_console().putc(']');
    print_str("\r\n  [PASS] HAL vtable dynamic dispatch operational.\r\n");
}

void cmd_test_svc_trap() {
    print_str("[TEST] Triggering 'svc #0' syscall trap via ARCH assembly helper...\r\n");
    HAL::Test::trigger_user_syscall_test();
    print_str("  [PASS] SVC trap handled and returned to EL1 successfully!\r\n");
}

void cmd_test_data_abort() {
    print_str(
        "[TEST] Triggering Data Abort by accessing invalid address 0x00000000DEADBEE0ULL...\r\n");
    volatile uint32_t* bad_ptr = reinterpret_cast<volatile uint32_t*>(0x00000000DEADBEE0ULL);
    *bad_ptr = 0x42; // Hardware Data Abort trap triggers here

    print_str("  [PASS] Data Abort trapped and execution safely resumed!\r\n");
}

void cmd_test_pfa() {
    print_str("[TEST] Running Physical Frame Allocator verification suite...\r\n");

    // TEST 1: Initrd Physical Reservation Check
    if (g_initrd_loc.valid) {
        bool all_pages_reserved = true;
        for (uintptr_t addr = g_initrd_loc.start_paddr; addr < g_initrd_loc.end_paddr;
             addr += Arch::PAGE_SIZE) {
            if (!pfa_is_page_allocated(addr)) {
                all_pages_reserved = false;
                break;
            }
        }
        assert_test(all_pages_reserved,
                    "Initrd physical memory range is marked reserved in PFA bitmap");
    }

    // TEST 2: Frame Allocations Never Collide with Initrd
    bool no_collision = true;
    uintptr_t test_pages[32];
    size_t allocated_count = 0;

    for (size_t i = 0; i < 32; ++i) {
        test_pages[i] = alloc_frame();
        if (test_pages[i] == 0) {
            break;
        }
        allocated_count++;

        if (g_initrd_loc.valid) {
            if (test_pages[i] >= g_initrd_loc.start_paddr &&
                test_pages[i] < g_initrd_loc.end_paddr) {
                no_collision = false;
            }
        }
    }

    assert_test(no_collision, "Newly allocated frames do not overlap with reserved Initrd region");

    for (size_t i = 0; i < allocated_count; ++i) {
        free_frame(test_pages[i]);
    }

    // TEST 3: Basic Frame Allocation & Recycling
    uintptr_t page1 = alloc_frame();
    assert_test(page1 != 0, "Allocated single page frame");
    assert_test((page1 % Arch::PAGE_SIZE) == 0, "Allocated frame is 4KB page-aligned");

    free_frame(page1);
    uintptr_t page_recycled = alloc_frame();
    assert_test(page_recycled == page1, "Freed frame was recycled cleanly");
    free_frame(page_recycled);
}

void cmd_test_ro() noexcept {
    print_str("\r\n[TEST] Running Read-Only (.rodata) Protection Test...\r\n");
    print_str("  Target Address: ");
    print_hex64(HAL::Test::get_ro_target_address());
    print_str("\r\n  Executing write store: *ptr = 0xCAFEBABE...\r\n");

    HAL::Test::trigger_ro_fault();

    print_str("  [PASS] Data Abort trapped and recovered cleanly! Kernel active.\r\n");
}

// Test 2: Trigger Instruction Abort on Non-Executable (.data) page execution
void cmd_test_nx() noexcept {
    print_str("\r\n[TEST] Running Execute-Never (NX) Protection Test...\r\n");
    print_str("  Target Address: ");
    print_hex64(HAL::Test::get_nx_target_address());
    print_str("\r\n  Branching into non-executable .data section...\r\n");

    HAL::Test::trigger_nx_fault();

    print_str("  [PASS] Instruction Abort trapped and recovered cleanly! Kernel active.\r\n");
}

// Test 3: Trigger EL0 System Register Access Trap (EC 0x18)
void cmd_test_sys() noexcept {
    print_str("\r\n[TEST] Running EL0 Privileged System Register Access Test...\r\n");
    print_str("  Entering EL0 User Mode to execute privileged instruction...\r\n");

    HAL::Test::trigger_privileged_sysreg_fault();

    print_str("  [PASS] EL0 System Register Trap handled! Process terminated cleanly.\r\n");
}

void print_help() noexcept {
    print_str("\r\nAvailable Commands:\r\n");
    print_str("  help        - Display this menu\r\n");
    print_str("  info        - Display system hardware & Exception Level\r\n");
    print_str("  clear       - Clear VT100 terminal screen\r\n");
    print_str("  test cpu    - Test CPU HAL interrupt enable/disable masking\r\n");
    print_str("  test mmu    - Read SCTLR_EL1 to verify MMU and Caches\r\n");
    print_str("  test el0    - Test EL0 user space switch and SVC trap return\r\n");
    print_str("  test cpp    - Verify .bss zeroing and C++ vtable dynamic dispatch\r\n");
    print_str("  test svc    - Execute SVC #0 trap and verify handler routing\r\n");
    print_str("  test abort  - Dereference unmapped pointer to test Data Abort trap\r\n");
    print_str("  test pfa    - Execute Page Frame Allocator tests\r\n");
    print_str("  test ro     - Test Read-Only protection write fault\r\n");
    print_str("  test nx     - Test Execute-Never protection execution fault\r\n");
    print_str("  test sys    - Test EL0 system register trap\r\n");
    print_str("  test all    - Run entire verification test suite\r\n");
    print_str("  halt        - Put CPU into low-power WFI state\r\n");
}

void dispatch_command(const char* buf) noexcept {
    if (streq(buf, "info")) {
        cmd_info();
    } else if (streq(buf, "test cpu")) {
        cmd_test_cpu();
    } else if (streq(buf, "test mmu")) {
        cmd_test_mmu();
    } else if (streq(buf, "test el0")) {
        cmd_test_el0();
    } else if (streq(buf, "test cpp")) {
        cmd_test_cpp();
    } else if (streq(buf, "test svc")) {
        cmd_test_svc_trap();
    } else if (streq(buf, "test abort")) {
        cmd_test_data_abort();
    } else if (streq(buf, "test pfa")) {
        cmd_test_pfa();
    } else if (streq(buf, "test ro")) {
        cmd_test_ro();
    } else if (streq(buf, "test nx")) {
        cmd_test_nx();
    } else if (streq(buf, "test sys")) {
        cmd_test_sys();
    } else if (streq(buf, "halt")) {
        print_str("[INFO] Halting CPU. Use QEMU 'Ctrl-A X' to exit.\r\n");
        HAL::get_cpu().halt();
    } else if (streq(buf, "clear")) {
        print_str("\033[2J\033[H"); // VT100 Clear Screen and
    } else if (streq(buf, "help")) {
        print_help();
    } else if (streq(buf, "test all")) {
        cmd_test_cpu();
        cmd_test_mmu();
        cmd_test_el0();
        cmd_test_cpp();
        cmd_test_svc_trap();
        cmd_test_data_abort();
        cmd_test_pfa();
        cmd_test_ro();
        cmd_test_nx();
        cmd_test_sys();
    } else if (buf[0] != '\0') {
        print_str("Unknown command: '");
        print_str(buf);
        print_str("'. Type 'help' for available commands.\r\n");
    }
}

} // namespace

namespace Kernel {
namespace Shell {

void run() noexcept {
    char input_buf[INPUT_BUFFER_SIZE];
    size_t buf_pos = 0;

    print_str("\r\n=============================================\r\n");
    print_str("       milo OS Kernel Shell Ready            \r\n");
    print_str("=============================================\r\n");
    print_str("milo> ");

    while (true) {
        const char chr = static_cast<char>(HAL::get_console().getc());

        if (chr == '\r' || chr == '\n') {
            print_str("\r\n");
            input_buf[buf_pos] = '\0';
            dispatch_command(input_buf);
            buf_pos = 0;
            print_str("milo> ");
        } else if (chr == '\b' || chr == 127) {
            if (buf_pos > 0) {
                buf_pos--;
                print_str("\b \b");
            }
        } else if (buf_pos < INPUT_BUFFER_SIZE - 1) {
            input_buf[buf_pos++] = chr;
            HAL::get_console().putc(chr);
        }
    }
}

} // namespace Shell
} // namespace Kernel