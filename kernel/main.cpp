#include "boot/fdt.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "shell/shell.hpp"

#include <cstdint>

uintptr_t g_dtb_paddr = 0;

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

    HAL::get_console().init();
    print_str("[KERNEL] Core HAL initialised successfully.\n");

    print_str("[BOOT] DTB Pointer: ");
    print_hex64(dtb_paddr);
    print_str("\n");

    if (FDT::is_valid(dtb_paddr)) {
        print_str("[FDT] Valid Device Tree Blob found. Total size: ");
        print_hex64(FDT::get_total_size(dtb_paddr));
        print_str(" bytes\n");

        auto initrd = FDT::find_initrd(dtb_paddr);
        if (initrd.valid) {
            print_str("[INITRD] Found via FDT /chosen node!\n");
            print_str("         Start: ");
            print_hex64(initrd.start_paddr);
            print_str("\n         End  : ");
            print_hex64(initrd.end_paddr);
            print_str("\n         Size : ");
            print_hex64(initrd.size);
            print_str(" bytes\n");

            // Verify CPIO magic header ("070701" or "070702")
            const auto* cpio_magic = reinterpret_cast<const char*>(initrd.start_paddr);
            print_str("         Signature: ");
            HAL::get_console().write(cpio_magic, 6);
            print_str("\n");
        } else {
            print_str("[INITRD] No initrd region found in /chosen.\n");
        }
    } else {
        print_str("[FDT ERROR] Invalid DTB magic at given address!\n");
    }

    HAL::get_cpu().disable_interrupts();
    Kernel::Shell::run();
}