use kernel_hal::{FrameAllocator, PhysAddr, PAGE_SIZE};

pub struct BitmapFrameAllocator<const SLOTS: usize> {
    start_addr: usize,
    bitmap: [u64; SLOTS],
}

impl<const SLOTS: usize> BitmapFrameAllocator<SLOTS> {
    pub const fn new(start_addr: usize) -> Self {
        Self {
            start_addr,
            bitmap: [0; SLOTS],
        }
    }
}

impl<const SLOTS: usize> FrameAllocator for BitmapFrameAllocator<SLOTS> {
    fn alloc_frame(&mut self) -> Option<PhysAddr> {
        for (i, slot) in self.bitmap.iter_mut().enumerate() {
            if *slot != u64::MAX {
                for bit in 0..64 {
                    if (*slot & (1 << bit)) == 0 {
                        *slot |= 1 << bit;
                        let frame_idx = i * 64 + bit;
                        return Some(PhysAddr(self.start_addr + (frame_idx * PAGE_SIZE)));
                    }
                }
            }
        }
        None
    }

    fn dealloc_frame(&mut self, frame: PhysAddr) {
        let frame_idx = (frame.0 - self.start_addr) / PAGE_SIZE;
        let i = frame_idx / 64;
        let bit = frame_idx % 64;
        if i < SLOTS {
            self.bitmap[i] &= !(1 << bit);
        }
    }
}