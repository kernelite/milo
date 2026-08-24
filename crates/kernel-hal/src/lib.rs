//! Kernel Hardware Abstraction Layer (HAL) interface.
//!
//! Provides foundational types and traits for architecture-agnostic memory
//! management, including physical/virtual address representations, page frame
//! allocation, and page table manipulation.

#![no_std]

/// Standard hardware page size in bytes (4 KiB).
pub const PAGE_SIZE: usize = 4096;

/// Represents a physical memory address in system RAM.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct PhysAddr(pub usize);

/// Represents a virtual memory address within the CPU's address space.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct VirtAddr(pub usize);

impl PhysAddr {
    /// Checks whether the physical address is aligned to a standard page boundary ([`PAGE_SIZE`]).
    ///
    /// Returns `true` if the raw address is a multiple of 4096 bytes, `false` otherwise.
    pub fn is_aligned(&self) -> bool {
        self.0 % PAGE_SIZE == 0
    }
}

/// Interface for physical frame memory allocators.
///
/// Implementations manage free physical memory frames (typically in 4 KiB chunks)
/// and supply them to sub-systems like page table managers or kernel heap initializers.
pub trait FrameAllocator {
    /// Allocates a single physical memory frame.
    ///
    /// Returns `Some(PhysAddr)` pointing to the start of the frame, or `None` if physical memory is exhausted.
    fn alloc_frame(&mut self) -> Option<PhysAddr>;

    /// Deallocates a previously allocated physical frame, returning it to the pool.
    ///
    /// # Parameters
    /// * `frame` - The starting physical address of the 4 KiB frame to release.
    fn dealloc_frame(&mut self, frame: PhysAddr);
}

/// Interface for controlling CPU-specific virtual memory page tables.
pub trait PageTable {
    /// Maps a virtual address to a physical address using the provided hardware flags.
    ///
    /// Uses the supplied [`FrameAllocator`] to allocate intermediate page table levels
    /// (e.g., page directories) if they do not yet exist along the path.
    ///
    /// # Parameters
    /// * `virt` - Target virtual address to map.
    /// * `phys` - Target physical address to link.
    /// * `flags` - Architecture-specific page attributes (e.g., Present, Writable, User-accessible, No-Execute).
    /// * `allocator` - Frame allocator used if intermediate table structures need creation.
    ///
    /// # Errors
    /// Returns `Err(())` if memory frame allocation fails during intermediate table creation
    /// or if the mapping invalidates hardware restrictions.
    fn map(
        &mut self,
        virt: VirtAddr,
        phys: PhysAddr,
        flags: u64,
        allocator: &mut impl FrameAllocator,
    ) -> Result<(), ()>;

    /// Removes an existing virtual address mapping from the page table.
    ///
    /// # Parameters
    /// * `virt` - Virtual address to unmap.
    ///
    /// # Errors
    /// Returns `Err(())` if the virtual address is not currently mapped.
    fn unmap(&mut self, virt: VirtAddr) -> Result<(), ()>;

    /// Activates this page table in CPU hardware by loading its root address into the MMU.
    ///
    /// Typically updates architecture control registers (e.g., writes `CR3` on x86_64,
    /// `TTBR0_EL1`/`TTBR1_EL1` on Arm64, or `satp` on RISC-V).
    fn activate(&self);
}