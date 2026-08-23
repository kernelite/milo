#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;
use core::cell::UnsafeCell;
use core::arch::asm;
use core::panic::PanicInfo;
use kernel_arch_aarch64::exceptions::enter_user_mode;
use kernel_arch_aarch64::mmu::{
    enable_mmu, init_mmu_hardware, is_mmu_enabled, AP_EL0, ATTR_DEVICE, ATTR_NORMAL, SH_INNER,
    Arm64PageTable,
};
use kernel_arch_aarch64::uart::{print, print_hex, uart_getc, uart_init, uart_putc};

pub mod allocator;
pub mod heap;

use allocator::BitmapFrameAllocator;
use heap::init_heap;
use kernel_hal::{FrameAllocator, PageTable};

extern "C" {
    fn user_space_code();
}

struct KernelStatic<T>(UnsafeCell<T>);
unsafe impl<T> Sync for KernelStatic<T> {}

static ROOT_PAGE_TABLE: KernelStatic<Arm64PageTable> =
    KernelStatic(UnsafeCell::new(Arm64PageTable::new()));

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
    print("\r\n[KERNEL PANIC]\r\n");
    loop {}
}

#[no_mangle]
pub extern "C" fn kmain() -> ! {
    uart_init();

    print("\r\n--- Microkernel Shell (Phase 3 User Mode) ---\r\n");

    let mut frame_allocator = BitmapFrameAllocator::<64>::new(0x4000_0000);
    init_heap(&mut frame_allocator, 16);

    init_mmu_hardware();

    unsafe {
        let page_table = &mut *ROOT_PAGE_TABLE.0.get();
        page_table.map_1gb_block(0x0000_0000, 0x0000_0000, ATTR_DEVICE);
        page_table.map_1gb_block(0x4000_0000, 0x4000_0000, ATTR_NORMAL | SH_INNER);
        page_table.map_1gb_block(0x8000_0000, 0x4000_0000, ATTR_NORMAL | SH_INNER | AP_EL0);
        
        page_table.activate();
        enable_mmu();
    }

    print("MMU & Exception Vector Configured.\r\n");
    print("Type 'help' for available commands.\r\n");

    let mut buffer = [0u8; 128];
    let mut cursor = 0;

    print("\r\nmon> ");

    loop {
        let b = uart_getc();

        if b == b'\r' || b == b'\n' {
            print("\r\n");
            if cursor > 0 {
                let cmd = &buffer[..cursor];
                if streq(cmd, b"help") {
                    print("Commands: help, ping, testheap, testmmu, testabi, testuser, info");
                } else if streq(cmd, b"ping") {
                    print("pong!");
                } else if streq(cmd, b"testheap") {
                    let mut vec = Vec::new();
                    vec.push(100);
                    vec.push(200);
                    if vec.len() == 2 && vec[1] == 200 {
                        print("Heap functional.");
                    }
                } else if streq(cmd, b"testmmu") {
                    if is_mmu_enabled() {
                        print("MMU Active!");
                    }
                } else if streq(cmd, b"testabi") {
                    let msg = "Hello via sys_write (Linux ABI x8=64)!\r\n";
                    let ret: u64;

                    unsafe {
                        asm!(
                            "svc #0",
                            in("x8") 64,
                            in("x0") 1,
                            in("x1") msg.as_ptr(),
                            in("x2") msg.len(),
                            lateout("x0") ret,
                        );
                    }

                    print("Syscall sys_write returned byte count: ");
                    print_hex(ret);
                } else if streq(cmd, b"testuser") {
                    print("Allocating user stack and jumping to EL0 User Mode...\r\n");
                    if let Some(frame) = frame_allocator.alloc_frame() {
                        let kernel_stack_top = frame.0 + 4096;
                        let user_code_va = (user_space_code as *const () as usize) + 0x4000_0000;
                        let user_stack_va = kernel_stack_top + 0x4000_0000;
                        let exit_code = unsafe { enter_user_mode(user_code_va, user_stack_va) };

                        print("User process returned control to shell with exit code: ");
                        print_hex(exit_code);
                    } else {
                        print("Failed to allocate user stack frame!");
                    }
                } else if streq(cmd, b"info") {
                    print("Arch: Arm64 | Board: QEMU virt | EL0 Isolation: Enabled");
                } else {
                    print("Unknown command");
                }
                cursor = 0;
                print("\r\n");
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