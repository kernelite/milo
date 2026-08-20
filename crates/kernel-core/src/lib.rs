#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;
use core::panic::PanicInfo;
use kernel_arch_aarch64::mmu::{
    enable_mmu, init_mmu_hardware, is_mmu_enabled, ATTR_DEVICE, ATTR_NORMAL, SH_INNER,
    Arm64PageTable,
};
use kernel_arch_aarch64::uart::{print, uart_getc, uart_init, uart_putc};

pub mod allocator;
pub mod heap;

use allocator::BitmapFrameAllocator;
use heap::init_heap;
use kernel_hal::PageTable;

static mut ROOT_PAGE_TABLE: Arm64PageTable = Arm64PageTable::new();

fn streq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    for i in 0..a.len() {
        if a[i] != b[i] {
            return false;
        }
    }
    true
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    print("\n[KERNEL PANIC]\n");
    loop {}
}

#[no_mangle]
pub extern "C" fn kmain() -> ! {
    uart_init();

    print("\n--- Microkernel Shell (Phase 1 Complete) ---\n");

    let mut frame_allocator = BitmapFrameAllocator::<64>::new(0x4000_0000);
    init_heap(&mut frame_allocator, 16);

    init_mmu_hardware();

    unsafe {
        // 1. Identity map 0x0000_0000 - 0x3FFF_FFFF (1GB Device Block for UART MMIO)
        ROOT_PAGE_TABLE.map_1gb_block(0x0000_0000, ATTR_DEVICE);

        // 2. Identity map 0x4000_0000 - 0x7FFF_FFFF (1GB Normal RAM Block for Kernel)
        ROOT_PAGE_TABLE.map_1gb_block(0x4000_0000, ATTR_NORMAL | SH_INNER);

        // 3. Load root table and enable MMU translation
        ROOT_PAGE_TABLE.activate();
        enable_mmu();
    }

    print("MMU Virtual Address Translation Enabled.\n");
    print("Type 'help' for available commands.\n");

    let mut buffer = [0u8; 128];
    let mut cursor = 0;

    print("\nmon> ");

    loop {
        let b = uart_getc();

        if b == b'\r' || b == b'\n' {
            print("\n");
            if cursor > 0 {
                let cmd = &buffer[..cursor];
                if streq(cmd, b"help") {
                    print("Commands: help, ping, testheap, testmmu, info");
                } else if streq(cmd, b"ping") {
                    print("pong!");
                } else if streq(cmd, b"testheap") {
                    let mut vec = Vec::new();
                    vec.push(100);
                    vec.push(200);
                    if vec.len() == 2 && vec[1] == 200 {
                        print("Dynamic heap allocation operating over MMU virtual memory!");
                    }
                } else if streq(cmd, b"testmmu") {
                    if is_mmu_enabled() {
                        print("MMU Active: SCTLR_EL1.M bit is SET (Translation Enabled)");
                    } else {
                        print("MMU Inactive!");
                    }
                } else if streq(cmd, b"info") {
                    print("Arch: Arm64 | Board: QEMU virt | MMU: Active | Heap: 64KB");
                } else {
                    print("Unknown command");
                }
                cursor = 0;
                print("\n");
            }
            print("mon> ");
        } else if b == 0x7F || b == 0x08 {
            if cursor > 0 {
                cursor -= 1;
                print("\x08 \x08");
            }
        } else if (32..=126).contains(&b) {
            if cursor < buffer.len() {
                buffer[cursor] = b;
                cursor += 1;
                uart_putc(b);
            }
        }
    }
}