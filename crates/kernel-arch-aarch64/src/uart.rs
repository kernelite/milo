const UART_BASE: usize = 0x0900_0000;
const UART_DR: *mut u32    = (UART_BASE + 0x00) as *mut u32;
const UART_FR: *const u32  = (UART_BASE + 0x18) as *const u32;
const UART_LCR_H: *mut u32 = (UART_BASE + 0x2C) as *mut u32;
const UART_CR: *mut u32    = (UART_BASE + 0x30) as *mut u32;

const FR_RXFE: u32 = 1 << 4;
const FR_TXFF: u32 = 1 << 5;

pub fn uart_init() {
    unsafe {
        UART_LCR_H.write_volatile((3 << 5) | (1 << 4));
        UART_CR.write_volatile((1 << 0) | (1 << 8) | (1 << 9));
    }
}

pub fn uart_putc(c: u8) {
    unsafe {
        while (UART_FR.read_volatile() & FR_TXFF) != 0 {}
        UART_DR.write_volatile(c as u32);
    }
}

pub fn uart_getc() -> u8 {
    unsafe {
        while (UART_FR.read_volatile() & FR_RXFE) != 0 {}
        (UART_DR.read_volatile() & 0xFF) as u8
    }
}

pub fn print(s: &str) {
    for byte in s.bytes() {
        if byte == b'\n' {
            uart_putc(b'\r');
        }
        uart_putc(byte);
    }
}

pub fn print_hex(val: u64) {
    print("0x");
    for i in (0..16).rev() {
        let nibble = (val >> (i * 4)) & 0xF;
        let c = if nibble < 10 {
            b'0' + nibble as u8
        } else {
            b'A' + (nibble - 10) as u8
        };
        uart_putc(c);
    }
}