#![no_std]
#![no_main]

use core::panic::PanicInfo;

const UART_BASE: usize = 0x0900_0000;

const UART_DR: *mut u32    = (UART_BASE + 0x00) as *mut u32;
const UART_FR: *const u32  = (UART_BASE + 0x18) as *const u32;
const UART_LCR_H: *mut u32 = (UART_BASE + 0x2C) as *mut u32;
const UART_CR: *mut u32    = (UART_BASE + 0x30) as *mut u32;

const FR_RXFE: u32 = 1 << 4; // Receive FIFO Empty
const FR_TXFF: u32 = 1 << 5; // Transmit FIFO Full

fn uart_init() {
    unsafe {
        UART_LCR_H.write_volatile((3 << 5) | (1 << 4));
        UART_CR.write_volatile((1 << 0) | (1 << 8) | (1 << 9));
    }
}

fn uart_putc(c: u8) {
    unsafe {
        while (UART_FR.read_volatile() & FR_TXFF) != 0 {}
        UART_DR.write_volatile(c as u32);
    }
}

fn uart_getc() -> u8 {
    unsafe {
        while (UART_FR.read_volatile() & FR_RXFE) != 0 {}
        (UART_DR.read_volatile() & 0xFF) as u8
    }
}

fn print(s: &str) {
    for byte in s.bytes() {
        if byte == b'\n' {
            uart_putc(b'\r');
        }
        uart_putc(byte);
    }
}

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

    print("\n--- Microkernel Shell (Arm64) ---\n");
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
                    print("Commands: help, ping, clear, info");
                } else if streq(cmd, b"ping") {
                    print("pong!");
                } else if streq(cmd, b"clear") {
                    print("\x1B[2J\x1B[H");
                } else if streq(cmd, b"info") {
                    print("Arch: Arm64 | Board: QEMU virt | Driver: PL011 UART");
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
        } else if b >= 32 && b <= 126 {
            if cursor < buffer.len() {
                buffer[cursor] = b;
                cursor += 1;
                uart_putc(b);
            }
        }
    }
}
