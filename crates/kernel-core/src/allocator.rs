//! Bitmap-Based Physical Frame Allocator.
//!
//! Provides a lightweight physical memory manager using a bitmask array to track frame availability.
//! Each bit in the bitmap tracks a single 4 KiB physical page frame (`0` = free, `1` = allocated).

use kernel_hal::{FrameAllocator, PhysAddr, PAGE_SIZE};

/// Physical page frame allocator backed by a fixed-size array bitmap.
///
/// Generic over `SLOTS`, representing the number of `u64` words in the bitmap.
/// Capable of tracking up to `SLOTS * 64` physical frames (`SLOTS * 256 KiB` total memory range).
pub struct BitmapFrameAllocator<const SLOTS: usize> {
    /// Base physical memory address where allocated frames begin.
    start_addr: usize,
    /// Bit array tracking allocation state (bit `0` = available, bit `1` = allocated).
    bitmap: [u64; SLOTS],
}

impl<const SLOTS: usize> BitmapFrameAllocator<SLOTS> {
    /// Creates a new, empty bitmap allocator rooted at `start_addr`.
    ///
    /// Initially, all managed frames are marked as available (`0`).
    ///
    /// # Parameters
    /// * `start_addr` - Base physical address where managed physical memory starts.
    pub const fn new(start_addr: usize) -> Self {
        Self {
            start_addr,
            bitmap: [0; SLOTS],
        }
    }
}

impl<const SLOTS: usize> FrameAllocator for BitmapFrameAllocator<SLOTS> {
    /// Allocates the first available 4 KiB physical frame.
    ///
    /// Iterates through the bitmap array looking for the first unallocated bit (`0`),
    /// marks it as allocated (`1`), and calculates its corresponding physical address.
    ///
    /// # Returns
    /// * `Some(PhysAddr)` - Address of allocated physical frame.
    /// * `None` - All managed physical frames are exhausted.
    fn alloc_frame(&mut self) -> Option<PhysAddr> {
        for (i, slot) in self.bitmap.iter_mut().enumerate() {
            // Skip fully populated 64-bit words to optimize allocation search
            if *slot != u64::MAX {
                for bit in 0..64 {
                    // Check if candidate bit position is unallocated (0)
                    if (*slot & (1 << bit)) == 0 {
                        // Mark frame as allocated (set bit to 1)
                        *slot |= 1 << bit;
                        
                        // Calculate absolute physical frame address
                        let frame_idx = i * 64 + bit;
                        return Some(PhysAddr(self.start_addr + (frame_idx * PAGE_SIZE)));
                    }
                }
            }
        }
        None
    }

    /// Deallocates a physical frame, returning it to the pool of available memory.
    ///
    /// # Parameters
    /// * `frame` - Base physical address of the 4 KiB frame to release.
    fn dealloc_frame(&mut self, frame: PhysAddr) {
        // Calculate frame index relative to managed base start address
        let frame_idx = (frame.0 - self.start_addr) / PAGE_SIZE;
        
        // Compute bitmap word index (`i`) and offset bit (`bit`)
        let i = frame_idx / 64;
        let bit = frame_idx % 64;

        // Ensure target bit offset lies within valid managed bounds before clearing
        if i < SLOTS {
            // Mark frame as unallocated (clear bit to 0)
            self.bitmap[i] &= !(1 << bit);
        }
    }
}