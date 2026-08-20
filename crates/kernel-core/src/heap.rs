use linked_list_allocator::LockedHeap;
use kernel_hal::{FrameAllocator, PAGE_SIZE};

#[global_allocator]
static ALLOCATOR: LockedHeap = LockedHeap::empty();

pub fn init_heap(frame_allocator: &mut impl FrameAllocator, pages: usize) {
    let mut heap_start = 0;

    // Allocate contiguous frames for the kernel heap
    for i in 0..pages {
        if let Some(frame) = frame_allocator.alloc_frame() {
            if i == 0 {
                heap_start = frame.0;
            }
        } else {
            panic!("Out of memory during heap initialization");
        }
    }

    let heap_size = pages * PAGE_SIZE;

    unsafe {
        ALLOCATOR.lock().init(heap_start as *mut u8, heap_size);
    }
}