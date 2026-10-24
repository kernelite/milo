#include "boot/fdt.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "mm/pfa.hpp"
#include "shell/shell.hpp"

#include <cstdint>

uintptr_t g_dtb_paddr = 0;
FDT::InitrdLocation g_initrd_loc{};

namespace {

void print_str(const char* str) noexcept {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    HAL::get_console().write(str, len);
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

} // namespace

extern "C" void kmain(uintptr_t dtb_paddr) {
    g_dtb_paddr = dtb_paddr;

    // 1. Initialize Console Driver
    HAL::get_console().init();
    print_str("[KERNEL] Core HAL initialised successfully.\r\n");

    // 2. Initialize Physical Frame Allocator via HAL Platform Query
    const auto platform = HAL::get_platform_info();
    pfa_init(platform.ram_base, platform.ram_size);

    // 3. Inspect DTB & Reserve DTB physical memory
    print_str("[BOOT] DTB Pointer: ");
    print_hex64(dtb_paddr);
    print_str("\r\n");

    if (FDT::is_valid(dtb_paddr)) {
        const uint32_t dtb_size = FDT::get_total_size(dtb_paddr);
        pfa_reserve_range(dtb_paddr, dtb_paddr + dtb_size);

        print_str("[FDT] DTB reserved in PFA: ");
        print_hex64(dtb_paddr);
        print_str(" - ");
        print_hex64(dtb_paddr + dtb_size);
        print_str("\r\n");

        // 4. Parse & Reserve Initrd
        g_initrd_loc = FDT::find_initrd(dtb_paddr);
        if (g_initrd_loc.valid) {
            pfa_reserve_range(g_initrd_loc.start_paddr, g_initrd_loc.end_paddr);

            print_str("[INITRD] Found via FDT /chosen!\r\n");
            print_str("         Start: ");
            print_hex64(g_initrd_loc.start_paddr);
            print_str("\r\n         End  : ");
            print_hex64(g_initrd_loc.end_paddr);
            print_str("\r\n         Size : ");
            print_hex64(g_initrd_loc.size);
            print_str(" bytes\r\n");

            const auto* cpio_magic = reinterpret_cast<const char*>(g_initrd_loc.start_paddr);
            print_str("         Signature: ");
            HAL::get_console().write(cpio_magic, 6);
            print_str("\r\n");
        } else {
            print_str("[INITRD WARNING] No initrd found in /chosen.\r\n");
        }
    } else {
        print_str("[FDT ERROR] Invalid DTB magic!\r\n");
    }

    // 5. Initialize MMU and Paging
    print_str("[KERNEL] Activating MMU and Translation Tables...\r\n");
    HAL::get_mmu().activate();
    print_str("[KERNEL] MMU Activated successfully.\r\n");

    // 6. Disable interrupts and drop into shell
    HAL::get_cpu().disable_interrupts();
    Kernel::Shell::run();
}