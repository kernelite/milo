//! AArch64 Exception and System Call Handling.
//!
//! Defines the register state layout ([`TrapFrame`]), foreign assembly entry points
//! for EL0 user-mode transitions, and the high-level Rust exception dispatcher
//! implementing Linux AArch64 ABI system calls.

use crate::uart::print_raw;

extern "C" {
    /// Transfers CPU execution from EL1 (Kernel Mode) to EL0 (User Mode).
    ///
    /// Saves current kernel execution context and jumps to `entry_point` with `user_stack_top`.
    ///
    /// # Parameters
    /// * `entry_point` - Virtual address of initial instruction to execute in EL0.
    /// * `user_stack_top` - Virtual address of top of user space stack (SP_EL0).
    ///
    /// # Returns
    /// The exit code passed when user process calls `SYS_EXIT`.
    pub fn enter_user_mode(entry_point: usize, user_stack_top: usize) -> u64;

    /// Restores saved kernel context from `enter_user_mode` and returns from exception loop.
    ///
    /// # Parameters
    /// * `exit_code` - Integer code returned back to kernel shell.
    fn return_to_kernel(exit_code: u64) -> !;
}

/// Saved CPU Register Context captured on exception vector entry.
///
/// Guaranteed 16-byte aligned layout per ARM64 ABI stack requirement.
/// Maps directly to assembly frame pushes/pops on kernel stack.
#[repr(C, align(16))]
pub struct TrapFrame {
    /// General-purpose registers `x0` through `x29` (sp + 0x000 .. 0x0EF).
    pub regs: [u64; 30],

    /// Link Register `x30` (`LR`) storing caller return address (sp + 0x0F0).
    pub lr: u64,

    /// Exception Link Register (`ELR_EL1`), target return instruction pointer (sp + 0x0F8).
    pub elr: u64,

    /// Saved Program Status Register (`SPSR_EL1`), captured CPU flags state (sp + 0x100).
    pub spsr: u64,

    /// Exception Syndrome Register (`ESR_EL1`), details exception cause (sp + 0x108).
    pub esr: u64,

    /// Fault Address Register (`FAR_EL1`), holds memory access fault address (sp + 0x110).
    pub far: u64,

    /// 128-bit SIMD / Floating-Point registers `v0` through `v31` (sp + 0x120 .. 0x31F).
    pub qregs: [u128; 32],
}

/// Linux AArch64 ABI System Call: `sys_write` (64)
pub const SYS_WRITE: u64 = 64;

/// Linux AArch64 ABI System Call: `sys_exit` (93)
pub const SYS_EXIT: u64 = 93;

/// Main high-level exception dispatcher invoked by assembly vector handlers.
///
/// Inspects CPU register context saved in `tf` to evaluate system calls or hardware traps.
///
/// # Parameters
/// * `tf` - Mutable reference to saved register frame on exception stack.
#[no_mangle]
pub extern "C" fn rust_exception_handler(tf: &mut TrapFrame) {
    // In Arm64 Linux ABI, x8 register carries system call ID
    let syscall_num = tf.regs[8];

    match syscall_num {
        SYS_WRITE => {
            // Arg 1 (x1): Buffer pointer | Arg 2 (x2): Length in bytes
            let buf = tf.regs[1] as *const u8;
            let len = tf.regs[2] as usize;

            // SAFETY: `buf` assumed valid slice provided by EL0 caller for `len` bytes
            unsafe {
                print_raw(buf, len);
            }

            // Return value passed in register x0
            tf.regs[0] = len as u64;
        }
        SYS_EXIT => {
            // Arg 0 (x0): Return exit status code
            let exit_code = tf.regs[0];

            // Restore saved kernel execution context recorded during `enter_user_mode`
            unsafe {
                return_to_kernel(exit_code);
            }
        }
        _ => {
            // Ignore unmapped or unknown syscall IDs
        }
    }
}