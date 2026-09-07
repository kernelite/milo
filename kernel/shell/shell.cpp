#include "shell.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include <cstddef>
#include <cstdint>

// Assembly routine declarations from boot.s
extern "C" {
    void user_space_code();
    int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp);
}

namespace {
    constexpr size_t MAX_CMD_LEN = 128;
    alignas(16) static uint8_t user_test_stack[2048];
    static int bss_check_var; // Uninitialized global variable to verify .bss zeroing

    bool streq(const char* s1, const char* s2) {
        while (*s1 && (*s1 == *s2)) {
            s1++;
            s2++;
        }
        return *s1 == *s2;
    }

    void print(const char* str) {
        size_t len = 0;
        while (str[len] != '\0') {
            len++;
        }
        HAL::console.write(str, len);
    }

    void print_hex64(uint64_t val) {
        char buf[19] = "0x0000000000000000";
        const char hex_chars[] = "0123456789ABCDEF";
        for (int i = 17; i >= 2; --i) {
            buf[i] = hex_chars[val & 0xF];
            val >>= 4;
        }
        print(buf);
    }

    // --- TEST SUITES ---

    void test_cpu() {
        print("[TEST] Testing CPU HAL Interrupt Management...\r\n");
        bool initial_state = HAL::cpu.interrupts_enabled();

        HAL::cpu.disable_interrupts();
        bool disabled_state = HAL::cpu.interrupts_enabled();
        print(disabled_state ? "  [FAIL] Interrupts still enabled after disable()\r\n"
                             : "  [PASS] Interrupts successfully disabled.\r\n");

        HAL::cpu.enable_interrupts();
        bool enabled_state = HAL::cpu.interrupts_enabled();
        print(enabled_state ? "  [PASS] Interrupts successfully enabled.\r\n"
                            : "  [FAIL] Interrupts still disabled after enable()\r\n");

        // Restore initial state
        if (!initial_state) {
            HAL::cpu.disable_interrupts();
        }
    }

    void test_mmu() {
        print("[TEST] Inspecting System MMU & Cache Status via HAL...\r\n");

        HAL::MmuStatus *status = new HAL::MmuStatus();

        print("  Control Register Value: ");
        print_hex64(status->raw_control_reg);
        print("\r\n");

        print(status->mmu_enabled    ? "  [PASS] MMU: ENABLED\r\n"             : "  [INFO] MMU: DISABLED\r\n");
        print(status->dcache_enabled ? "  [PASS] Data Cache: ENABLED\r\n"      : "  [INFO] Data Cache: DISABLED\r\n");
        print(status->icache_enabled ? "  [PASS] Instruction Cache: ENABLED\r\n": "  [INFO] Instruction Cache: DISABLED\r\n");
    }

    void test_el0() {
        print("[TEST] Testing EL0 User-Mode Transition & SVC Trap...\r\n");
        uintptr_t stack_top = reinterpret_cast<uintptr_t>(user_test_stack) + sizeof(user_test_stack);

        print("  Entering EL0 user_space_code at ");
        print_hex64(reinterpret_cast<uintptr_t>(user_space_code));
        print("...\r\n");

        // Jump to EL0; user_space_code executes SVC #0 and returns via return_to_kernel
        int64_t res = enter_user_mode(reinterpret_cast<uintptr_t>(user_space_code), stack_top);

        print("  [PASS] Safely returned from EL0 to EL1! Exit status: ");
        print_hex64(static_cast<uint64_t>(res));
        print("\r\n");
    }

    void test_cpp() {
        print("[TEST] Verifying Freestanding C++ Runtime...\r\n");

        if (bss_check_var == 0) {
            print("  [PASS] .bss section correctly zero-initialized.\r\n");
        } else {
            print("  [FAIL] .bss contains uninitialized garbage!\r\n");
        }

        print("  Testing Virtual Function Dynamic Dispatch... ");
        HAL::console.putc('[');
        HAL::console.putc('O');
        HAL::console.putc('K');
        HAL::console.putc(']');
        print("\r\n  [PASS] HAL vtable dynamic dispatch operational.\r\n");
    }

    void execute_command(char* cmd) {
        if (cmd[0] == '\0') {
            return;
        } else if (streq(cmd, "help")) {
            print("Available Commands:\r\n");
            print("  help        - Display this menu\r\n");
            print("  info        - Display system hardware & Exception Level\r\n");
            print("  clear       - Clear VT100 terminal screen\r\n");
            print("  test cpu    - Test CPU HAL interrupt enable/disable masking\r\n");
            print("  test mmu    - Read SCTLR_EL1 to verify MMU and Caches\r\n");
            print("  test el0    - Test EL0 user space switch and SVC trap return\r\n");
            print("  test cpp    - Verify .bss zeroing and C++ vtable dynamic dispatch\r\n");
            print("  test all    - Run entire verification test suite\r\n");
            print("  halt        - Put CPU into low-power WFI state\r\n");
        } else if (streq(cmd, "info")) {
            uint8_t el = HAL::cpu.current_el(); // Clean HAL Call!

            print("Architecture : AArch64 (QEMU virt, Cortex-A53)\r\n");
            print("Current EL   : EL");
            HAL::console.putc(static_cast<char>('0' + el));
            print("\r\nHAL Driver   : PL011 UART MMIO @ 0x09000000\r\n");
        } else if (streq(cmd, "clear")) {
            print("\033[2J\033[H");
        } else if (streq(cmd, "test cpu")) {
            test_cpu();
        } else if (streq(cmd, "test mmu")) {
            test_mmu();
        } else if (streq(cmd, "test el0")) {
            test_el0();
        } else if (streq(cmd, "test cpp")) {
            test_cpp();
        } else if (streq(cmd, "test all")) {
            test_cpp();
            test_cpu();
            test_mmu();
            test_el0();
        } else if (streq(cmd, "halt")) {
            print("Halting CPU...\r\n");
            while (true) {
                HAL::cpu.halt();
            }
        } else {
            print("Unknown command: ");
            print(cmd);
            print("\r\nType 'help' for available commands.\r\n");
        }
    }
}

namespace Kernel {
    void Shell::run() {
        print("\r\n========================================\r\n");
        print("      AArch64 Bare-Metal C++ Shell      \r\n");
        print("========================================\r\n");
        print("Type 'help' or 'test all' to begin.\r\n\r\n");

        char buf[MAX_CMD_LEN];
        size_t pos = 0;

        print("milo> ");

        while (true) {
            char c = static_cast<char>(HAL::console.getc());

            if (c == '\r' || c == '\n') {
                print("\r\n");
                buf[pos] = '\0';
                execute_command(buf);
                pos = 0;
                print("milo> ");
            } else if (c == 0x08 || c == 0x7F) { // Backspace
                if (pos > 0) {
                    pos--;
                    print("\b \b");
                }
            } else if (c >= 32 && c <= 126) {
                if (pos < MAX_CMD_LEN - 1) {
                    buf[pos++] = c;
                    HAL::console.putc(c);
                }
            }
        }
    }
}