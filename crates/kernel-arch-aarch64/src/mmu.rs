use core::arch::asm;
use kernel_hal::{FrameAllocator, PageTable, PhysAddr, VirtAddr};

const ENTRY_COUNT: usize = 512;

// Descriptor Bit Flags for Arm64 L1 Block (1GB)
pub const VALID: u64        = 1 << 0;
pub const BLOCK: u64        = 0 << 1; // Bit 1 = 0 for 1GB Block Descriptor in L1
pub const ATTR_DEVICE: u64  = 0 << 2; // MAIR Attribute Index 0
pub const ATTR_NORMAL: u64  = 1 << 2; // MAIR Attribute Index 1
pub const AP_EL0: u64       = 1 << 6; // Unprivileged (EL0) Read/Write Access
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

    pub fn map_1gb_block(&mut self, virt_base: usize, phys_base: usize, flags: u64) {
        let index = virt_base >> 30;
        if index < ENTRY_COUNT {
            let phys_addr = (phys_base as u64) & !0x3FFF_FFFF;
            self.entries[index] = phys_addr | VALID | BLOCK | AF | flags;
        }
    }
}

impl PageTable for Arm64PageTable {
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
            asm!("msr ttbr0_el1, {}", in(reg) phys_addr);
            asm!("dsb sy", "tlbi vmalle1is", "dsb sy", "isb");
        }
    }
}

pub fn init_mmu_hardware() {
    unsafe {
        // MAIR_EL1: Attr 0 = Device-nGnRnE (0x00), Attr 1 = Normal WB Memory (0xFF)
        let mair: u64 = (0x00 << 0) | (0xFF << 8);
        asm!("msr mair_el1, {}", in(reg) mair);

        // TCR_EL1: T0SZ = 25 (39-bit VA space), TG0 = 4KB, Non-cacheable table walks
        let tcr: u64 = 25 | (0 << 8) | (0 << 10) | (3 << 12) | (2u64 << 32);
        asm!("msr tcr_el1, {}", in(reg) tcr);
    }
}

pub fn enable_mmu() {
    unsafe {
        let mut sctlr: u64;
        asm!("mrs {}, sctlr_el1", out(reg) sctlr);
        sctlr |= 1 << 0;     // M bit: Enable MMU virtual address translation
        sctlr &= !(1 << 2);  // Disable C bit (Data Cache) to prevent cache incoherence
        sctlr &= !(1 << 12); // Disable I bit (Instruction Cache) to prevent stale vector fetches
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