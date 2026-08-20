use core::arch::asm;
use kernel_hal::{FrameAllocator, PageTable, PhysAddr, VirtAddr};

const ENTRY_COUNT: usize = 512;

// Descriptor Bit Flags for Arm64 L1 Block (1GB)
pub const VALID: u64        = 1 << 0;
pub const BLOCK: u64        = 0 << 1; // Bit 1 = 0 for 1GB Block Descriptor in L1
pub const ATTR_DEVICE: u64  = 0 << 2; // MAIR Attribute Index 0
pub const ATTR_NORMAL: u64  = 1 << 2; // MAIR Attribute Index 1
pub const SH_INNER: u64     = 3 << 8; // Inner Shareable
pub const AF: u64           = 1 << 10; // Access Flag (Prevents Access Faults)

#[repr(C, align(4096))]
pub struct Arm64PageTable {
    pub entries: [u64; ENTRY_COUNT],
}

impl Arm64PageTable {
    pub const fn new() -> Self {
        Self { entries: [0; ENTRY_COUNT] }
    }

    /// Identity map a 1GB region at `base_addr`
    pub fn map_1gb_block(&mut self, base_addr: usize, flags: u64) {
        let index = base_addr >> 30; // 1GB boundary index (bits 38:30)
        if index < ENTRY_COUNT {
            // Mask out lower 30 bits (0x3FFF_FFFF) to align address to 1GB
            let phys_base = (base_addr as u64) & !0x3FFF_FFFF;
            self.entries[index] = phys_base | VALID | BLOCK | AF | flags;
        }
    }
}

impl PageTable for Arm64PageTable {
    fn map(
        &mut self,
        virt: VirtAddr,
        phys: PhysAddr,
        flags: u64,
        _allocator: &mut impl FrameAllocator,
    ) -> Result<(), ()> {
        self.map_1gb_block(virt.0, flags);
        Ok(())
    }

    fn unmap(&mut self, virt: VirtAddr) -> Result<(), ()> {
        let index = virt.0 >> 30;
        if index < ENTRY_COUNT {
            self.entries[index] = 0;
        }
        Ok(())
    }

    fn activate(&self) {
        let phys_addr = self as *const _ as usize as u64;
        unsafe {
            // Set TTBR0_EL1 to the root table address
            asm!("msr ttbr0_el1, {}", in(reg) phys_addr);
            // Invalidate TLB and synchronize memory barrier
            asm!("dsb sy", "tlbi vmalle1is", "dsb sy", "isb");
        }
    }
}

pub fn init_mmu_hardware() {
    unsafe {
        // MAIR_EL1: Attr 0 = Device-nGnRnE (0x00), Attr 1 = Normal WB Memory (0xFF)
        let mair: u64 = (0x00 << 0) | (0xFF << 8);
        asm!("msr mair_el1, {}", in(reg) mair);

        // TCR_EL1: T0SZ = 25 (39-bit VA space, L1 Root Table), TG0 = 4KB, Inner Shareable
        let tcr: u64 = 25 | (1 << 8) | (1 << 10) | (3 << 12) | (2u64 << 32);
        asm!("msr tcr_el1, {}", in(reg) tcr);
    }
}

pub fn enable_mmu() {
    unsafe {
        let mut sctlr: u64;
        asm!("mrs {}, sctlr_el1", out(reg) sctlr);
        sctlr |= 1 << 0;  // M bit: Enable MMU address translation
        sctlr |= 1 << 2;  // C bit: Enable Data Cache
        sctlr |= 1 << 12; // I bit: Enable Instruction Cache
        asm!("msr sctlr_el1, {}", in(reg) sctlr);
        asm!("dsb sy", "isb");
    }
}

pub fn is_mmu_enabled() -> bool {
    let sctlr: u64;
    unsafe {
        asm!("mrs {}, sctlr_el1", out(reg) sctlr);
    }
    (sctlr & (1 << 0)) != 0
}