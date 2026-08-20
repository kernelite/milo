use crate::uart::{print, print_hex};

#[repr(C)]
pub struct TrapFrame {
    pub regs: [u64; 30], // x0 - x29
    pub lr: u64,         // x30
    pub elr: u64,        // Fault PC
    pub spsr: u64,       // Saved PSTATE
    pub esr: u64,        // Exception Syndrome
    pub far: u64,        // Fault Address
}

#[no_mangle]
pub extern "C" fn rust_exception_handler(tf: &mut TrapFrame) {
    let esr_ec = (tf.esr >> 26) & 0x3F;

    if esr_ec == 0x15 {
        print("\n[TRAP] SVC #0 Captured by VBAR_EL1!\n");
        tf.elr += 4; // Advance past 'svc' instruction
        return;
    }

    print("\n[UNHANDLED FAULT] EC=");
    print_hex(esr_ec);
    print(" | ELR=");
    print_hex(tf.elr);
    print(" | FAR=");
    print_hex(tf.far);
    print("\nSystem Halted.\n");

    loop {
        unsafe {
            core::arch::asm!("wfe");
        }
    }
}