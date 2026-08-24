//! AArch64 Page Table and MMU Hardware Management.
//!
//! Provides level-1 (1 GiB block) page table abstractions and low-level control register
//! setup for ARMv8-A Memory Management Unit (MMU) initialization, translation table activation,
//! and virtual memory configuration.

use core::arch::asm;
use kernel_hal::{FrameAllocator, PageTable, PhysAddr, VirtAddr};

/// Number of translation descriptors per 4 KiB page table page (512 * 8 bytes = 4096 bytes).
const ENTRY_COUNT: usize = 512;

// --- AArch64 L1 Block Descriptor Bit Flags (1 GiB Mapping) ---

/// Bit 0: Valid Descriptor Flag. Indicates if the entry is valid for MMU translation.
pub const VALID: u64 = 1 << 0;

/// Bit 1: Block Descriptor Flag (0 = 1 GiB Block Descriptor in Level 1 Table, 1 = Table Descriptor).
pub const BLOCK: u64 = 0 << 1;

/// Bits 2..4: Memory Attribute Index 0 (points to MAIR_EL1 Attr 0: Device-nGnRnE memory).
pub const ATTR_DEVICE: u64 = 0 << 2;

/// Bits 2..4: Memory Attribute Index 1 (points to MAIR_EL1 Attr 1: Normal Cacheable memory).
pub const ATTR_NORMAL: u64 = 1 << 2;

/// Bit 6: Access Permission Bit (1 = Read/Write permission accessible at EL0 User Mode).
pub const AP_EL0: u64 = 1 << 6;

/// Bits 8..9: Shareability Domain (3 = Inner Shareable for multi-core cache coherency).
pub const SH_INNER: u64 = 3 << 8;

/// Bit 10: Access Flag (1 = Page has been accessed; prevents hardware Access Flag faults).
pub const AF: u64 = 1 << 10;

/// Represents an AArch64 Level-1 Translation Table aligned to a 4 KiB boundary.
#[repr(C, align(4096))]
pub struct Arm64PageTable {
    /// Array of 512 64-bit page table entries (PTEs).
    pub entries: [u64; ENTRY_COUNT],
}

impl Arm64PageTable {
    /// Creates a zero-initialized Level-1 page table with all invalid entries.
    pub const fn new() -> Self {
        Self { entries: [0; ENTRY_COUNT] }
    }

    /// Identity/Directly maps a 1 GiB block of physical memory at a target virtual base address.
    ///
    /// # Parameters
    /// * `virt_base` - Virtual starting address (must be 1 GiB aligned).
    /// * `phys_base` - Physical starting address to map (must be 1 GiB aligned).
    /// * `flags` - Configuration attributes (e.g., memory type, permissions).
    pub fn map_1gb_block(&mut self, virt_base: usize, phys_base: usize, flags: u64) {
        // Compute L1 index by extracting bits 30..38 (virt_base / 1 GB)
        let index = virt_base >> 30;
        if index < ENTRY_COUNT {
            // Mask out low 30 bits to ensure 1 GiB physical address alignment
            let phys_addr = (phys_base as u64) & !0x3FFF_FFFF;
            // Assemble 64-bit descriptor: Physical Address + Descriptor Control Flags
            self.entries[index] = phys_addr | VALID | BLOCK | AF | flags;
        }
    }
}

impl PageTable for Arm64PageTable {
    /// Maps a virtual address to a physical address using a 1 GiB block mapping.
    ///
    /// Implements [`kernel_hal::PageTable`].
    fn map(
        &mut self,
        virt: VirtAddr,
        _phys: PhysAddr,
        flags: u64,
        _allocator: &mut impl FrameAllocator,
    ) -> Result<(), ()> {
        self.map_1gb_block(virt.0, virt.0, flags);
        Ok(())
    }

    /// Clears an existing 1 GiB translation entry at the specified virtual address.
    ///
    /// Implements [`kernel_hal::PageTable`].
    fn unmap(&mut self, virt: VirtAddr) -> Result<(), ()> {
        let index = virt.0 >> 30;
        if index < ENTRY_COUNT {
            self.entries[index] = 0; // Mark entry invalid
        }
        Ok(())
    }

    /// Loads this page table base address into `TTBR0_EL1` and flushes the TLB.
    ///
    /// Implements [`kernel_hal::PageTable`].
    fn activate(&self) {
        let phys_addr = self as *const _ as usize as u64;
        unsafe {
            // Set Translation Table Base Register 0 for EL1/EL0 translations
            asm!("msr ttbr0_el1, {}", in(reg) phys_addr);
            
            // System synchronization barrier followed by TLB invalidation and instruction barrier
            asm!(
                "dsb sy",          // Ensure TTBR0 write completes before invalidation
                "tlbi vmalle1is",  // Invalidate all stage-1 TLB entries for Inner Shareable domain
                "dsb sy",          // Wait for TLB invalidation completion
                "isb"              // Flush pipeline to fetch instructions with new translation context
            );
        }
    }
}

/// Configures AArch64 memory attribute and translation control registers.
///
/// Sets up:
/// * `MAIR_EL1` (Memory Attribute Indirection Register): Defines Device and Normal memory attributes.
/// * `TCR_EL1` (Translation Control Register): Sets 39-bit virtual address space (512 GiB) and 4 KiB granules.
pub fn init_mmu_hardware() {
    unsafe {
        // Configure Memory Attribute Indirection Register (MAIR_EL1):
        // Index 0 (0x00): Device-nGnRnE (Non-gathering, Non-reordering, Non-early-write-ack)
        // Index 1 (0xFF): Normal Inner/Outer Write-Back Cacheable memory
        let mair: u64 = (0x00 << 0) | (0xFF << 8);
        asm!("msr mair_el1, {}", in(reg) mair);

        // Configure Translation Control Register (TCR_EL1):
        // T0SZ = 25    -> 64 - 25 = 39-bit Virtual Address space (TTBR0)
        // IRGN0 = 0    -> TTBR0 Inner Non-cacheable table walks
        // ORGN0 = 0    -> TTBR0 Outer Non-cacheable table walks
        // SH0 = 3      -> Inner Shareable attribute for table walks
        // TG0 = 0      -> 4 KiB page granule size for TTBR0
        // IPS = 2      -> 40-bit Intermediate Physical Address size (1 TB max PA)
        let tcr: u64 = 25 | (0 << 8) | (0 << 10) | (3 << 12) | (2u64 << 32);
        asm!("msr tcr_el1, {}", in(reg) tcr);
    }
}

/// Enables the MMU in system control register `SCTLR_EL1`.
///
/// Activates virtual address translation while keeping caches temporarily disabled
/// to maintain coherency during early kernel execution.
pub fn enable_mmu() {
    unsafe {
        let mut sctlr: u64;
        // Read current System Control Register (EL1) value
        asm!("mrs {}, sctlr_el1", out(reg) sctlr);
        
        sctlr |= 1 << 0;     // Bit 0 (M): Enable MMU address translation
        sctlr &= !(1 << 2);  // Bit 2 (C): Disable Data Cache (prevents incoherence before cache setup)
        sctlr &= !(1 << 12); // Bit 12 (I): Disable Instruction Cache (prevents stale vector fetches)
        
        // Write back updated configuration to SCTLR_EL1
        asm!("msr sctlr_el1, {}", in(reg) sctlr);
        
        // Synchronize memory and instruction pipeline changes
        asm!("dsb sy", "isb");
    }
}

/// Checks whether the MMU is currently active.
///
/// Reads bit 0 (`M` bit) of `SCTLR_EL1`.
///
/// # Returns
/// `true` if MMU virtual memory translation is enabled, `false` otherwise.
pub fn is_mmu_enabled() -> bool {
    let sctlr: u64;
    unsafe {
        asm!("mrs {}, sctlr_el1", out(reg) sctlr);
    }
    (sctlr & (1 << 0)) != 0
}