//! Microkernel Core Entry Point and Monitor Shell.
//!
//! This module implements the main kernel entry point (`kmain`) for an AArch64 (Arm64) 
//! microkernel running on bare-metal target hardware (e.g., QEMU `virt`).
//! It sets up early UART I/O, initial frame allocation, dynamic heap management,
//! page table identity/user mappings via the MMU, and an interactive debug shell with
//! user-mode (EL0) context switching capabilities.

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
    /// External symbol defined in assembly representing the user space entry point.
    fn user_space_code();
}

/// A interior-mutability wrapper used to host mutable singletons in global statics
/// prior to formal concurrency/locking implementation.
///
/// # Safety
/// Wrapping types with `Sync` allows multi-threaded access; here it is used safely 
/// during single-core boot sequence initialization.
struct KernelStatic<T>(UnsafeCell<T>);
unsafe impl<T> Sync for KernelStatic<T> {}

/// Global identity root page table storage.
static ROOT_PAGE_TABLE: KernelStatic<Arm64PageTable> =
    KernelStatic(UnsafeCell::new(Arm64PageTable::new()));

/// Compares two byte slices for equality.
///
/// Returns `true` if both slices have identical length and byte contents.
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

/// Architectural panic handler triggered on unrecoverable kernel errors.
///
/// Outputs a panic diagnostic to UART and enters an infinite halt loop.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    print("\r\n[KERNEL PANIC]\r\n");
    loop {}
}

/// Core Kernel Entry Point called by architecture boot assembly (`_start`).
///
/// Performs hardware subsystem initialization, configures memory translation,
/// and executes the kernel interactive diagnostic shell.
#[no_mangle]
pub extern "C" fn kmain() -> ! {
    // Initialize UART hardware for early diagnostic logging
    uart_init();

    print("\r\n--- Microkernel Shell (Phase 3 User Mode) ---\r\n");

    // Initialize physical frame allocator starting at RAM base address 0x4000_0000 (QEMU virt RAM start)
    let mut frame_allocator = BitmapFrameAllocator::<64>::new(0x4000_0000);
    
    // Reserve 16 physical pages (64 KiB) to back the dynamic kernel heap allocator
    init_heap(&mut frame_allocator, 16);

    // Prepare AArch64 CPU registers (MAIR_EL1, TCR_EL1) for translation
    init_mmu_hardware();

    // Map kernel and user space virtual memory ranges into the root page table
    unsafe {
        let page_table = &mut *ROOT_PAGE_TABLE.0.get();
        
        // 0x0000_0000 - 0x3FFF_FFFF (1 GB): Device Memory (MMIO peripherals like UART)
        page_table.map_1gb_block(0x0000_0000, 0x0000_0000, ATTR_DEVICE);
        
        // 0x4000_0000 - 0x7FFF_FFFF (1 GB): Kernel Normal Memory (RAM, Read/Write EL1)
        page_table.map_1gb_block(0x4000_0000, 0x4000_0000, ATTR_NORMAL | SH_INNER);
        
        // 0x8000_0000 - 0xBFFF_FFFF (1 GB): User Space Memory (RAM alias accessible from EL0)
        page_table.map_1gb_block(0x8000_0000, 0x4000_0000, ATTR_NORMAL | SH_INNER | AP_EL0);
        
        // Set TTBR0_EL1 register to point to root table layout
        page_table.activate();
        
        // Enable virtual memory address translation via SCTLR_EL1.M bit
        enable_mmu();
    }

    print("MMU & Exception Vector Configured.\r\n");
    print("Type 'help' for available commands.\r\n");

    let mut buffer = [0u8; 128];
    let mut cursor = 0;

    print("\r\nmon> ");

    // Main Interactive UART Shell Loop
    loop {
        let b = uart_getc();

        // Handle Return / Enter Key press
        if b == b'\r' || b == b'\n' {
            print("\r\n");
            if cursor > 0 {
                let cmd = &buffer[..cursor];
                
                // --- Shell Command Evaluation ---
                if streq(cmd, b"help") {
                    print("Commands: help, ping, testheap, testmmu, testabi, testuser, info");
                } else if streq(cmd, b"ping") {
                    print("pong!");
                } else if streq(cmd, b"testheap") {
                    // Test dynamic memory allocation via Rust's alloc::vec::Vec
                    let mut vec = Vec::new();
                    vec.push(100);
                    vec.push(200);
                    if vec.len() == 2 && vec[1] == 200 {
                        print("Heap functional.");
                    }
                } else if streq(cmd, b"testmmu") {
                    // Query system register flags to verify MMU state
                    if is_mmu_enabled() {
                        print("MMU Active!");
                    }
                } else if streq(cmd, b"testabi") {
                    // Issue a Linux-compatible system call (sys_write = 64) via AArch64 software interrupt (svc #0)
                    let msg = "Hello via sys_write (Linux ABI x8=64)!\r\n";
                    let ret: u64;

                    unsafe {
                        asm!(
                            "svc #0",
                            in("x8") 64,             // Syscall number (sys_write)
                            in("x0") 1,              // File Descriptor 1 (stdout)
                            in("x1") msg.as_ptr(),   // Buffer pointer
                            in("x2") msg.len(),      // Byte count
                            lateout("x0") ret,       // Return value from handler
                        );
                    }

                    print("Syscall sys_write returned byte count: ");
                    print_hex(ret);
                } else if streq(cmd, b"testuser") {
                    // Prepare and transition CPU execution state from EL1 (Kernel) to EL0 (User Mode)
                    print("Allocating user stack and jumping to EL0 User Mode...\r\n");
                    
                    if let Some(frame) = frame_allocator.alloc_frame() {
                        let kernel_stack_top = frame.0 + 4096;
                        
                        // Remap user code and stack addresses into EL0-accessible physical alias region (0x8000_0000 base offset)
                        let user_code_va = (user_space_code as *const () as usize) + 0x4000_0000;
                        let user_stack_va = kernel_stack_top + 0x4000_0000;
                        
                        // Execute drop to EL0; returns execution to kernel upon system call/exit exception
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
        } 
        // Handle Backspace / Delete Keys
        else if b == 0x7F || b == 0x08 {
            if cursor > 0 {
                cursor -= 1;
                print("\x08 \x08"); // Send Backspace-Space-Backspace sequence to clear character on terminal
            }
        } 
        // Buffer Printable ASCII Characters
        else if (32..=126).contains(&b) {
            if cursor < buffer.len() {
                buffer[cursor] = b;
                cursor += 1;
                uart_putc(b); // Echo back character to console
            }
        }
    }
}