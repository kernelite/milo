//! Kernel Heap Initialization and Global Allocator Setup.
//!
//! Provides global memory allocation support for `#![no_std]` environment
//! primitives (such as `alloc::vec::Vec` and `alloc::boxed::Box`) by wrapping
//! a thread-safe linked-list allocator initialized with memory frames.

use linked_list_allocator::LockedHeap;
use kernel_hal::{FrameAllocator, PAGE_SIZE};

/// Global thread-safe heap allocator backing all dynamic kernel allocations.
///
/// Implements `core::alloc::GlobalAlloc` via `LockedHeap`, which utilizes internal
/// spinlocks to synchronize concurrent allocation requests across kernel contexts.
#[global_allocator]
static ALLOCATOR: LockedHeap = LockedHeap::empty();

/// Initializes the global kernel heap with a specified capacity of physical memory frames.
///
/// Requests physical frames from the supplied [`FrameAllocator`] and presents the 
/// resulting contiguous address region to the global [`LockedHeap`] instance.
///
/// # Parameters
/// * `frame_allocator` - Mutable reference to the system's physical frame allocator.
/// * `pages` - Number of contiguous 4 KiB frames ([`PAGE_SIZE`]) to reserve for the heap.
///
/// # Panics
/// Panics if physical memory is exhausted before the requested number of pages can be reserved.
pub fn init_heap(frame_allocator: &mut impl FrameAllocator, pages: usize) {
    let mut heap_start = 0;

    // Request physical frames to establish the underlying heap buffer region
    for i in 0..pages {
        if let Some(frame) = frame_allocator.alloc_frame() {
            // Capture the base memory address of the first frame as the heap's starting pointer
            if i == 0 {
                heap_start = frame.0;
            }
        } else {
            panic!("Out of memory during heap initialization");
        }
    }

    // Calculate total heap region size in bytes
    let heap_size = pages * PAGE_SIZE;

    // SAFETY: `heap_start` points to a valid, unmanaged region of memory of size `heap_size`
    // allocated exclusively for this heap. The memory region is non-null and unused.
    unsafe {
        ALLOCATOR.lock().init(heap_start as *mut u8, heap_size);
    }
}