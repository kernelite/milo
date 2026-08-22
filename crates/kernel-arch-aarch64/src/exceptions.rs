use crate::uart::print_raw;

extern "C" {
    pub fn enter_user_mode(entry_point: usize, user_stack_top: usize) -> u64;
    fn return_to_kernel(exit_code: u64) -> !;
}

#[repr(C, align(16))]
pub struct TrapFrame {
    pub regs: [u64; 30],  // sp + 0x000 .. 0x0ef
    pub lr: u64,          // sp + 0x0f0
    pub elr: u64,         // sp + 0x0f8
    pub spsr: u64,        // sp + 0x100
    pub esr: u64,         // sp + 0x108
    pub far: u64,         // sp + 0x110
    pub qregs: [u128; 32],// sp + 0x120 .. 0x31f
}

pub const SYS_WRITE: u64 = 64;
pub const SYS_EXIT:  u64 = 93;

#[no_mangle]
pub extern "C" fn rust_exception_handler(tf: &mut TrapFrame) {
    let syscall_num = tf.regs[8]; // x8

    match syscall_num {
        64 => { // SYS_WRITE
            let buf = tf.regs[1] as *const u8;
            let len = tf.regs[2] as usize;

            unsafe {
                print_raw(buf, len);
            }

            tf.regs[0] = len as u64; // Return bytes written
            tf.elr += 4;         // Skip 'svc #0' instruction
        }
        93 => { // SYS_EXIT
            let exit_code = tf.regs[0]; // x0

            tf.elr += 4;         // Skip 'svc #0' instruction
            // Jump directly back to kernel_ctx saved in enter_user_mode
            unsafe {
                return_to_kernel(exit_code);
            }
        }
        _ => {
            tf.elr += 4;
        }
    }
}