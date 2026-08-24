//! Kernel Hardware Abstraction Layer (HAL) Core Traits.
//!
//! Defines target-agnostic traits for CPU control, virtual memory management,
//! trap/exception handling, and early serial debugging console I/O.

#![no_std]

use core::fmt;

/// Standard hardware page size in bytes (4 KiB).
pub const PAGE_SIZE: usize = 4096;

/// Represents a physical memory address in system RAM.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct PhysAddr(pub usize);

/// Represents a virtual memory address within the CPU address space.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct VirtAddr(pub usize);

impl PhysAddr {
    /// Checks whether the physical address is aligned to a page boundary ([`PAGE_SIZE`]).
    pub fn is_aligned(&self) -> bool {
        self.0 % PAGE_SIZE == 0
    }
}

impl VirtAddr {
    /// Checks whether the virtual address is aligned to a page boundary ([`PAGE_SIZE`]).
    pub fn is_aligned(&self) -> bool {
        self.0 % PAGE_SIZE == 0
    }
}

// =========================================================================
// 1. CPU Control & Context Traits
// =========================================================================

/// Interface for low-level CPU state control and execution flow management.
pub trait Cpu {
    /// Enables hardware interrupts on the calling CPU core.
    fn enable_interrupts();

    /// Disables hardware interrupts on the calling CPU core.
    fn disable_interrupts();

    /// Returns `true` if interrupts are currently enabled on the calling CPU core.
    fn interrupts_enabled() -> bool;

    /// Puts the CPU core into a low-power wait-for-interrupt state (e.g., `wfi` on ARM, `hlt` on x86).
    fn wait_for_interrupt();

    /// Unconditionally halts the current CPU core in a dead loop with interrupts disabled.
    fn halt() -> ! {
        Self::disable_interrupts();
        loop {
            Self::wait_for_interrupt();
        }
    }
}

/// Architecture-agnostic CPU register state saved during context switches.
pub trait CpuContext: Sized {
    /// Initializes a fresh thread execution context.
    ///
    /// # Parameters
    /// * `entry_point` - Initial instruction address where thread execution begins.
    /// * `stack_top` - Virtual pointer to the top of the thread's stack.
    /// * `is_user` - `true` if configuring an EL0/User context; `false` for EL1/Kernel thread.
    fn new(entry_point: usize, stack_top: usize, is_user: bool) -> Self;

    /// Switches execution context from `self` (current saved state) to `next`.
    ///
    /// # Safety
    /// Directly manipulates CPU stack pointers, program counters, and general-purpose registers.
    unsafe fn switch_to(&mut self, next: &Self);
}

// =========================================================================
// 2. Virtual Memory Management Traits
// =========================================================================

/// Interface for physical frame allocators.
pub trait FrameAllocator {
    /// Allocates a single physical memory frame (4 KiB).
    fn alloc_frame(&mut self) -> Option<PhysAddr>;

    /// Deallocates a previously allocated physical frame.
    fn dealloc_frame(&mut self, frame: PhysAddr);
}

/// Target-agnostic interface for CPU page table structures.
pub trait PageTable {
    /// Maps a virtual address page to a physical memory address frame using hardware flags.
    ///
    /// # Parameters
    /// * `virt` - Target virtual address.
    /// * `phys` - Target physical address frame.
    /// * `flags` - Target architecture page flags (e.g., Read/Write, User, Executable).
    /// * `allocator` - Frame allocator used if intermediate page sub-tables must be allocated.
    fn map(
        &mut self,
        virt: VirtAddr,
        phys: PhysAddr,
        flags: u64,
        allocator: &mut impl FrameAllocator,
    ) -> Result<(), ()>;

    /// Unmaps a virtual address mapping from the page table.
    fn unmap(&mut self, virt: VirtAddr) -> Result<(), ()>;

    /// Activates this page table in hardware (e.g., updates MMU root register `TTBR0_EL1` / `CR3`).
    fn activate(&self);
}

// =========================================================================
// 3. Trap & Exception Handling Traits
// =========================================================================

/// Generic classification of processor faults, system calls, and interrupt exceptions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrapKind {
    /// User space system call requested via software interrupt (e.g., `svc #0` / `syscall`).
    Syscall { number: u64 },
    /// Memory translation or permission fault.
    PageFault { addr: VirtAddr, is_write: bool },
    /// Memory access alignment failure.
    AlignmentFault,
    /// Invalid opcode or instruction execution trap.
    IllegalInstruction,
    /// Unhandled hardware exception or external interrupt vector.
    Unhandled { vector: u64 },
}

/// Interface for handling processor traps, faults, and system calls.
pub trait TrapHandler {
    /// Dispatches and processes an exception captured by hardware vector routines.
    ///
    /// # Parameters
    /// * `trap` - Hardware-agnostic classification of the trap event.
    /// * `context` - Saved register frame of the faulting/calling thread context.
    ///
    /// # Returns
    /// `Ok(())` if the trap was handled successfully and execution can resume,
    /// or `Err(())` if the fault is fatal to the execution context.
    fn handle_trap(&mut self, trap: TrapKind, context: &mut impl CpuContext) -> Result<(), ()>;
}

// =========================================================================
// 4. Early Debug Console Trait
// =========================================================================

/// Hardware interface for basic early-stage serial output and debugging.
pub trait EarlyConsole: fmt::Write {
    /// Initializes early hardware console (e.g., UART clock and pin settings).
    fn init(&mut self);

    /// Outputs a single raw byte to the console channel.
    fn write_byte(&mut self, byte: u8);

    /// Reads a single raw byte from the console channel (blocking).
    fn read_byte(&mut self) -> u8;

    /// Flushes any pending output buffers in hardware.
    fn flush(&mut self);
}