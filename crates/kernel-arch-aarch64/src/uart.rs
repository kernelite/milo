//! ARM PrimeCell PL011 UART Driver.
//!
//! Provides bare-metal driver routines for serial input/output via Memory-Mapped I/O (MMIO).
//! Configured for the standard PL011 UART peripheral base address on the QEMU `virt` board (`0x0900_0000`).

/// Base physical memory-mapped address for the PL011 UART peripheral (QEMU `virt` board).
const UART_BASE: usize = 0x0900_0000;

/// Data Register (Read: received data byte, Write: transmit data byte).
const UART_DR: *mut u32 = (UART_BASE + 0x00) as *mut u32;

/// Flag Register (Read-only status flags for TX/RX FIFO buffers).
const UART_FR: *const u32 = (UART_BASE + 0x18) as *const u32;

/// Line Control Register (Configures word length, parity, and FIFOs).
const UART_LCR_H: *mut u32 = (UART_BASE + 0x2C) as *mut u32;

/// Control Register (Enables UART peripheral, transmitter, and receiver hardware).
const UART_CR: *mut u32 = (UART_BASE + 0x30) as *mut u32;

/// Flag Register Bit 4: Receive FIFO Empty (`1` when empty, `0` when character available).
const FR_RXFE: u32 = 1 << 4;

/// Flag Register Bit 5: Transmit FIFO Full (`1` when full, `0` when space available).
const FR_TXFF: u32 = 1 << 5;

/// Initializes the PL011 UART device hardware.
///
/// Configures 8-bit word length, enables internal hardware FIFOs, and activates
/// both transmit (TX) and receive (RX) units.
pub fn uart_init() {
    unsafe {
        // LCR_H: Set 8-bit word length (bits 5:6 = 11 -> 3 << 5) and enable FIFOs (bit 4 -> 1 << 4)
        UART_LCR_H.write_volatile((3 << 5) | (1 << 4));

        // CR: Enable UART (bit 0), Transmit Enable (bit 8), and Receive Enable (bit 9)
        UART_CR.write_volatile((1 << 0) | (1 << 8) | (1 << 9));
    }
}

/// Transmits a single byte over UART using polling (blocking).
///
/// Blocks execution until space becomes available in the transmit FIFO buffer before writing.
///
/// # Parameters
/// * `c` - ASCII character byte to transmit.
pub fn uart_putc(c: u8) {
    unsafe {
        // Poll Flag Register until Transmit FIFO Full (TXFF) flag clears
        while (UART_FR.read_volatile() & FR_TXFF) != 0 {}

        // Write character byte to Data Register
        UART_DR.write_volatile(c as u32);
    }
}

/// Receives a single byte from UART using polling (blocking).
///
/// Blocks execution until a byte arrives in the receive FIFO buffer.
///
/// # Returns
/// The received ASCII character byte.
pub fn uart_getc() -> u8 {
    unsafe {
        // Poll Flag Register until Receive FIFO Empty (RXFE) flag clears
        while (UART_FR.read_volatile() & FR_RXFE) != 0 {}

        // Read character byte from Data Register (mask out status bits 8..11)
        (UART_DR.read_volatile() & 0xFF) as u8
    }
}

/// Prints a UTF-8 string slice to the UART console.
///
/// Automatically translates line feed characters (`\n`) to carriage-return/line-feed (`\r\n`)
/// sequences required by standard terminal emulators.
///
/// # Parameters
/// * `s` - String slice to display.
pub fn print(s: &str) {
    for byte in s.bytes() {
        if byte == b'\n' {
            uart_putc(b'\r');
        }
        uart_putc(byte);
    }
}

/// Prints a raw byte buffer to UART directly from a raw memory pointer.
///
/// Performs carriage return translation (`\n` -> `\r\n`) for line endings.
///
/// # Parameters
/// * `ptr` - Pointer to base of the byte buffer.
/// * `len` - Length of buffer in bytes.
///
/// # Safety
/// Caller must ensure `ptr` points to a valid readable buffer of at least `len` bytes.
pub unsafe fn print_raw(ptr: *const u8, len: usize) {
    for i in 0..len {
        let byte = *ptr.add(i);
        if byte == b'\n' {
            uart_putc(b'\r');
        }
        uart_putc(byte);
    }
}

/// Formats and prints a 64-bit integer as a zero-padded 16-digit hexadecimal string.
///
/// Output is prefixed with `0x` (e.g., `0x0000000040000000`).
///
/// # Parameters
/// * `val` - 64-bit unsigned integer to print in hex format.
pub fn print_hex(val: u64) {
    print("0x");
    // Iterate through all 16 4-bit nibbles from most to least significant
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