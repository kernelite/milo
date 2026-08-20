use core::arch::asm;
use crate::uart::{print, print_hex, uart_putc};

extern "C" {
    pub fn enter_user_mode(entry_point: usize, user_stack_top: usize) -> u64;
    pub fn return_to_kernel();
}

#[repr(C, align(16))]
pub struct TrapFrame {
    pub regs: [u64; 31], // x0 - x30
    pub elr: u64,        // Faulting PC
    pub spsr: u64,       // Saved PSTATE
    pub esr: u64,        // Exception Syndrome
    pub far: u64,        // Fault Address
}

pub const SYS_WRITE: u64 = 64;
pub const SYS_EXIT:  u64 = 93;

#[no_mangle]
pub extern "C" fn rust_exception_handler(tf: &mut TrapFrame) {
    let esr_ec = (tf.esr >> 26) & 0x3F;

    if esr_ec == 0x15 {
        let syscall_num = tf.regs[8];

        match syscall_num {
            SYS_WRITE => {
                let fd = tf.regs[0];
                let buf_ptr = tf.regs[1] as *const u8;
                let count = tf.regs[2] as usize;

                if fd == 1 || fd == 2 {
                    unsafe {
                        let slice = core::slice::from_raw_parts(buf_ptr, count);
                        for &byte in slice {
                            if byte == b'\n' {
                                uart_putc(b'\r');
                            }
                            uart_putc(byte);
                        }
                    }
                    tf.regs[0] = count as u64;
                } else {
                    tf.regs[0] = (-1i64) as u64;
                }

                tf.spsr = 0x3c0;
                tf.elr += 4; // Advance past 'svc #0'
                return;
            }
            SYS_EXIT => {
                let code = tf.regs[0];
                print("\r\n[SYSCALL] Process called sys_exit with code: ");
                print_hex(code);
                print("\r\n[KERNEL] Restoring kernel execution context...\r\n");

                // Divert eret execution to return_to_kernel in EL1h mode
                tf.elr = return_to_kernel as *const () as usize as u64;
                tf.spsr = 0x000003c5; // EL1h mode with DAIF masked
                tf.regs[0] = code;
                return;
            }
            _ => {
                print("\r\n[SYSCALL] Unknown Syscall ID\r\n");
                tf.regs[0] = (-38i64) as u64;
                tf.spsr = 0x3c0;
                tf.elr += 4;
                return;
            }
        }
    }

    print("\r\n[UNHANDLED FAULT] EC=");
    print_hex(esr_ec);
    print(" | ELR=");
    print_hex(tf.elr);
    print(" | FAR=");
    print_hex(tf.far);
    print("\r\nSystem Halted.\r\n");

    loop {
        unsafe {
            asm!("wfe");
        }
    }
}