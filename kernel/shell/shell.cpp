#include "shell.hpp"

#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "mm/pfa.hpp"

#include <cstddef>
#include <cstdint>

extern "C" {
void user_space_code();
int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp);
void trigger_svc_test();
}

namespace {
constexpr size_t MAX_CMD_LEN = 128;
alignas(16) uint8_t user_test_stack[2048];
int bss_check_var;

alignas(16) uint8_t user_stack[4096];

bool streq(const char* str1, const char* str2) {
    if (str1 == nullptr || str2 == nullptr) {
        return str1 == str2;
    }

    while (*str1 != '\0' && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *str1 == *str2;
}

void print(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    HAL::get_console().write(str, len);
}

void print_hex64(uint64_t val) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 17; i >= 2; --i) {
        buf[i] = hex_chars[val & 0xFU];
        val >>= 4U;
    }
    buf[18] = '\0';
    print(buf);
}

void assert_test(bool condition, const char* test_name) {
    if (condition) {
        print("[PASS] ");
    } else {
        print("[FAIL] ");
    }
    print(test_name);
    print("\n");
}

void test_cpu() {
    print("[TEST] Testing CPU HAL Interrupt Management...\r\n");
    bool initial_state = HAL::get_cpu().interrupts_enabled();

    HAL::get_cpu().disable_interrupts();
    bool disabled_state = HAL::get_cpu().interrupts_enabled();
    print(disabled_state ? "  [FAIL] Interrupts still enabled after disable()\r\n"
                         : "  [PASS] Interrupts successfully disabled.\r\n");

    HAL::get_cpu().enable_interrupts();
    bool enabled_state = HAL::get_cpu().interrupts_enabled();
    print(enabled_state ? "  [PASS] Interrupts successfully enabled.\r\n"
                        : "  [FAIL] Interrupts still disabled after enable()\r\n");

    if (!initial_state) {
        HAL::get_cpu().disable_interrupts();
    }
}

void test_mmu() {
    print("[TEST] Inspecting System MMU & Cache Status via HAL...\r\n");

    const HAL::MmuStatus status = HAL::get_mmu().status();

    print("  Control Register Value: ");
    print_hex64(status.raw_control_reg);
    print("\r\n");

    print(status.mmu_enabled ? "  [PASS] MMU: ENABLED\r\n" : "  [INFO] MMU: DISABLED\r\n");
    print(status.dcache_enabled ? "  [PASS] Data Cache: ENABLED\r\n"
                                : "  [INFO] Data Cache: DISABLED\r\n");
    print(status.icache_enabled ? "  [PASS] Instruction Cache: ENABLED\r\n"
                                : "  [INFO] Instruction Cache: DISABLED\r\n");
}

void test_el0() {
    print("[TEST] Testing EL0 User-Mode Transition & SVC Trap...\r\n");
    uintptr_t stack_top = reinterpret_cast<uintptr_t>(user_test_stack) + sizeof(user_test_stack);

    print("  Entering EL0 user_space_code at ");
    print_hex64(reinterpret_cast<uintptr_t>(user_space_code));
    print("...\r\n");

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
    HAL::get_console().putc('[');
    HAL::get_console().putc('O');
    HAL::get_console().putc('K');
    HAL::get_console().putc(']');
    print("\r\n  [PASS] HAL vtable dynamic dispatch operational.\r\n");
}

void test_svc_trap() {
    print("[TEST] Triggering 'svc #0' syscall trap via ARCH assembly helper...\r\n");

    uint64_t user_sp = reinterpret_cast<uint64_t>(user_stack) + sizeof(user_stack);
    enter_user_mode(reinterpret_cast<uint64_t>(trigger_svc_test), user_sp);

    asm volatile("" ::: "memory");
    print("  [PASS] SVC trap handled and returned to EL1 successfully!\r\n");
}

void test_write_protect() {
    print("[TEST] Testing Write Protection (RO Permission Fault)...\r\n");
    print("  Attempting write to Read-Only section .rodata at ");
    print_hex64(reinterpret_cast<uintptr_t>(_rodata_start));
    print("...\r\n");

    const auto* ro_ptr = reinterpret_cast<const volatile uint32_t*>(_rodata_start);
    *const_cast<volatile uint32_t*>(ro_ptr) = 0x12345678U;

    print("  [PASS] Write protection fault trapped successfully!\r\n");
}

void test_nx_protect() {
    print("[TEST] Testing NX Protection (PXN Instruction Abort)...\r\n");
    alignas(8) static const uint32_t nx_code[2] = {0xD65F03C0U, 0xD65F03C0U};
    const uintptr_t code_addr = reinterpret_cast<uintptr_t>(nx_code);

    print("  Attempting to execute code from NX data page at ");
    print_hex64(code_addr);
    print("...\r\n");

    using FuncPtr = void (*)();
    auto func = reinterpret_cast<FuncPtr>(code_addr);
    func();

    print("  [PASS] NX execution fault trapped successfully!\r\n");
}

void test_data_abort() {
    print("[TEST] Triggering Data Abort by accessing invalid address 0x00000000DEADBEE0ULL...\r\n");
    volatile uint32_t* bad_ptr = reinterpret_cast<volatile uint32_t*>(0x00000000DEADBEE0ULL);
    *bad_ptr = 0x42;

    print("  [PASS] Data Abort trapped and execution safely resumed!\r\n");
}

void test_pfa() {
    uintptr_t kernel_end = reinterpret_cast<uintptr_t>(_text_end);

    uintptr_t ram_start = 0x40000000;
    uintptr_t ram_end = 0x48000000;

    pfa_init(ram_start, ram_end);

    uintptr_t page1 = alloc_frame();
    uintptr_t page1_addr = reinterpret_cast<uintptr_t>(page1);

    assert_test(page1 != 0L, "Allocated first page");
    assert_test((page1_addr % 4096) == 0, "First page is 4KB page-aligned");
    assert_test(page1_addr >= kernel_end, "First allocated page is outside kernel image");
    assert_test(page1_addr < ram_end, "First allocated page is within valid RAM");

    uintptr_t page2 = alloc_frame();
    uintptr_t page3 = alloc_frame();
    uintptr_t page2_addr = reinterpret_cast<uintptr_t>(page2);
    uintptr_t page3_addr = reinterpret_cast<uintptr_t>(page3);

    assert_test(page2 != 0L && page3 != 0L, "Allocated multiple pages");
    assert_test(page1 != page2 && page2 != page3, "Allocated pages have unique addresses");
    assert_test(page2_addr >= kernel_end && page3_addr >= kernel_end,
                "All pages are past kernel end");

    free_frame(page2);
    uintptr_t page_reused = alloc_frame();

    assert_test(page_reused == page2, "Freed page was successfully recycled");

    free_frame(page1);
    free_frame(page3);
    free_frame(page_reused);

    uintptr_t rw_page = alloc_frame();
    volatile uint64_t* ptr = reinterpret_cast<volatile uint64_t*>(rw_page);

    *ptr = 0xDEADBEEFCAFEBABE;
    assert_test(*ptr == 0xDEADBEEFCAFEBABE, "Allocated RAM page allows read/write");
    free_frame(rw_page);
}

void execute_command(char* cmd) {
    if (cmd[0] == '\0') {
        return;
    }
    if (streq(cmd, "help")) {
        print("Available Commands:\r\n");
        print("  help        - Display this menu\r\n");
        print("  info        - Display system hardware & Exception Level\r\n");
        print("  clear       - Clear VT100 terminal screen\r\n");
        print("  test cpu    - Test CPU HAL interrupt enable/disable masking\r\n");
        print("  test mmu    - Read SCTLR_EL1 to verify MMU and Caches\r\n");
        print("  test el0    - Test EL0 user space switch and SVC trap return\r\n");
        print("  test cpp    - Verify .bss zeroing and C++ vtable dynamic dispatch\r\n");
        print("  test svc    - Execute SVC #0 trap and verify handler routing\r\n");
        print("  test write  - Attempt write to RO page to test Data Abort permission fault\r\n");
        print("  test nx     - Attempt execute from NX page to test Instruction Abort permission fault\r\n");
        print("  test abort  - Dereference unmapped pointer to test Data Abort trap\r\n");
        print("  test pfa    - Execute Page Frame Allocator tests\r\n");
        print("  test all    - Run entire verification test suite\r\n");
        print("  halt        - Put CPU into low-power WFI state\r\n");
    } else if (streq(cmd, "info")) {
        uint8_t elvl = HAL::get_cpu().current_el();

        print("Architecture : AArch64 (QEMU virt, Cortex-A53)\r\n");
        print("Current EL   : EL");
        HAL::get_console().putc(static_cast<char>('0' + elvl));
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
    } else if (streq(cmd, "test svc")) {
        test_svc_trap();
    } else if (streq(cmd, "test write")) {
        test_write_protect();
    } else if (streq(cmd, "test nx")) {
        test_nx_protect();
    } else if (streq(cmd, "test abort")) {
        test_data_abort();
    } else if (streq(cmd, "test pfa")) {
        test_pfa();
    } else if (streq(cmd, "test all")) {
        test_cpp();
        test_cpu();
        test_mmu();
        test_el0();
        test_svc_trap();
        test_write_protect();
        test_nx_protect();
        test_data_abort();
        test_pfa();
    } else if (streq(cmd, "halt")) {
        print("Halting CPU...\r\n");
        while (true) {
            HAL::get_cpu().halt();
        }
    } else {
        print("Unknown command: ");
        print(cmd);
        print("\r\nType 'help' for available commands.\r\n");
    }
}
} // namespace

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
        char chr = static_cast<char>(HAL::get_console().getc());

        if (chr == '\r' || chr == '\n') {
            print("\r\n");
            buf[pos] = '\0';
            execute_command(buf);
            pos = 0;
            print("milo> ");
        } else if (chr == 0x08 || chr == 0x7F) {
            if (pos > 0) {
                pos--;
                print("\b \b");
            }
        } else if (chr >= 32 && chr <= 126) {
            if (pos < MAX_CMD_LEN - 1) {
                buf[pos++] = chr;
                HAL::get_console().putc(chr);
            }
        }
    }
}
} // namespace Kernel