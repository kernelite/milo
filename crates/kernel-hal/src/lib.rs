#![no_std]

pub const PAGE_SIZE: usize = 4096;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct PhysAddr(pub usize);

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct VirtAddr(pub usize);

impl PhysAddr {
    pub fn is_aligned(&self) -> bool {
        self.0 % PAGE_SIZE == 0
    }
}

pub trait FrameAllocator {
    fn alloc_frame(&mut self) -> Option<PhysAddr>;
    fn dealloc_frame(&mut self, frame: PhysAddr);
}

pub trait PageTable {
    fn map(
        &mut self,
        virt: VirtAddr,
        phys: PhysAddr,
        flags: u64,
        allocator: &mut impl FrameAllocator,
    ) -> Result<(), ()>;

    fn unmap(&mut self, virt: VirtAddr) -> Result<(), ()>;
    
    /// Activates table in CPU hardware (e.g., writes TTBR0_EL1 on Arm64 or CR3 on x86_64)
    fn activate(&self);
}