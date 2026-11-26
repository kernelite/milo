#include "mm/heap.hpp"

#include "memory_config.hpp"
#include "mm/AddressSpace.hpp"
#include "mm/pfa.hpp"
#include "sync/IrqGuard.hpp"

#include <cstdint>

#ifdef HOST_TEST
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#else
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/test_hooks.hpp"
#include "process/Process.hpp"
#include "process/scheduler.hpp"
#endif

namespace Kernel::MM {

namespace {

#ifdef HOST_TEST
std::mutex g_host_heap_mutex;
#endif

class HeapLockGuard final {
  public:
    HeapLockGuard() noexcept = default;
    ~HeapLockGuard() noexcept = default;

    HeapLockGuard(const HeapLockGuard&) = delete;
    HeapLockGuard& operator=(const HeapLockGuard&) = delete;
    HeapLockGuard(HeapLockGuard&&) = delete;
    HeapLockGuard& operator=(HeapLockGuard&&) = delete;

  private:
#ifdef HOST_TEST
    std::lock_guard<std::mutex> m_lock{g_host_heap_mutex};
#else
    Sync::IrqGuard m_irq_guard;
#endif
};

constinit KernelHeap g_kernel_heap{};

[[noreturn]] void heap_panic(const char* msg) noexcept {
#ifdef HOST_TEST
    std::fprintf(stderr, "%s", msg);
    std::abort();
#else
    size_t len = 0U;
    while (msg[len] != '\0') {
        ++len;
    }
    HAL::get_console().write(msg, len);
    while (true) {
        HAL::get_cpu().halt();
    }
#endif
}

constexpr bool is_power_of_two(size_t val) noexcept {
    return val != 0U && (val & (val - 1U)) == 0U;
}

constexpr uint16_t size_to_class_index(size_t effective_size) noexcept {
    uint16_t idx = 0U;
    size_t slot_size = KernelHeap::MIN_SLAB_SIZE;
    while (slot_size < effective_size && (idx + 1U) < KernelHeap::NUM_SLAB_CLASSES) {
        slot_size <<= 1U;
        ++idx;
    }
    return idx;
}

inline void slab_set_next(void* slot, void* next) noexcept {
    const auto next_addr = reinterpret_cast<uintptr_t>(next);
    __builtin_memcpy(slot, &next_addr, sizeof(next_addr));
}

inline void* slab_get_next(const void* slot) noexcept {
    uintptr_t next_addr = 0U;
    __builtin_memcpy(&next_addr, slot, sizeof(next_addr));
    return reinterpret_cast<void*>(next_addr);
}

struct RefcountStressWorkerArgs {
    uintptr_t shared_paddr{0U};
    uint32_t iterations{0U};
    uint32_t completed_iterations{0U};
};

void refcount_stress_worker_entry(uintptr_t raw_arg) noexcept {
    auto* const args = reinterpret_cast<RefcountStressWorkerArgs*>(raw_arg);
    if (args == nullptr || args->shared_paddr == 0U) {
        return;
    }

    for (uint32_t i = 0U; i < args->iterations; ++i) {
        page_get(args->shared_paddr);
        page_put(args->shared_paddr);
    }

    // Release this worker's hold on the shared frame
    page_put(args->shared_paddr);
    __atomic_store_n(&args->completed_iterations, args->iterations, __ATOMIC_RELEASE);
}

} // namespace

KernelHeap& KernelHeap::instance() noexcept {
    return g_kernel_heap;
}

void KernelHeap::list_push_front(Page*& head, Page* page) noexcept {
    page->set_list_links(nullptr, head);
    if (head != nullptr) {
        head->set_list_links(page, head->list_next());
    }
    head = page;
}

void KernelHeap::list_remove(Page*& head, Page* page) noexcept {
    Page* const prev_page = page->list_prev();
    Page* const next_page = page->list_next();
    if (prev_page != nullptr) {
        prev_page->set_list_links(prev_page->list_prev(), next_page);
    } else if (head == page) {
        head = next_page;
    }
    if (next_page != nullptr) {
        next_page->set_list_links(prev_page, next_page->list_next());
    }
    page->clear_list_links();
}

Page* KernelHeap::create_slab_page(SlabCache& cache) noexcept {
    const uintptr_t paddr = alloc_frame();
    if (paddr == 0U) {
        return nullptr;
    }

    Page* const page = page_from_paddr(paddr);
    if (page == nullptr) {
        free_frame(paddr);
        return nullptr;
    }

    auto* const base = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(paddr));
    void* freelist_head = nullptr;
    for (size_t i = cache.slots_per_page; i > 0U; --i) {
        void* const slot = base + ((i - 1U) * cache.slot_size);
        slab_set_next(slot, freelist_head);
        freelist_head = slot;
    }
    page->configure_slab(cache.class_idx, &cache, freelist_head);

    m_stats.slab_pages++;
    return page;
}

void* KernelHeap::slab_alloc_locked(SlabCache& cache) noexcept {
    Page* page = cache.partial_slabs;
    if (page == nullptr) {
        if (cache.reserve_slab != nullptr) {
            page = cache.reserve_slab;
            cache.reserve_slab = nullptr;
        } else {
            page = create_slab_page(cache);
            if (page == nullptr) {
                return nullptr;
            }
        }
        list_push_front(cache.partial_slabs, page);
    }

    void* const obj = page->freelist_head();
    if (obj == nullptr) {
        heap_panic("\r\n[PANIC] Corrupted slab freelist\r\n");
    }

    page->set_freelist_head(slab_get_next(obj));
    page->increment_slab_ref();

    if (page->slab_ref_count() == cache.slots_per_page) {
        list_remove(cache.partial_slabs, page);
        list_push_front(cache.full_slabs, page);
    }

    m_stats.active_slab_objects++;
    m_stats.active_bytes += cache.slot_size;
    return obj;
}

void KernelHeap::slab_free_locked(Page* page, uintptr_t paddr, void* ptr) noexcept {
    const uint16_t class_idx = page->slab_class_index();
    if (class_idx >= NUM_SLAB_CLASSES) {
        heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
    }

    SlabCache& cache = m_caches[class_idx];
    if (page->slab_cache() != &cache) {
        heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
    }

    if ((paddr & (cache.slot_size - 1U)) != 0U || page->slab_ref_count() == 0U ||
        page->slab_ref_count() > cache.slots_per_page) {
        heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
    }

    const bool was_full = (page->slab_ref_count() == cache.slots_per_page);

    slab_set_next(ptr, page->freelist_head());
    page->set_freelist_head(ptr);
    page->decrement_slab_ref();

    m_stats.active_slab_objects--;
    m_stats.active_bytes -= cache.slot_size;

    if (was_full) {
        list_remove(cache.full_slabs, page);
        if (page->slab_ref_count() > 0U) {
            list_push_front(cache.partial_slabs, page);
        } else if (cache.reserve_slab == nullptr) {
            cache.reserve_slab = page;
        } else {
            const uintptr_t frame_paddr = page_to_paddr(page);
            page->release_slab_to_allocated();
            m_stats.slab_pages--;
            free_frame(frame_paddr);
        }
        return;
    }

    if (page->slab_ref_count() == 0U) {
        list_remove(cache.partial_slabs, page);
        if (cache.reserve_slab == nullptr) {
            cache.reserve_slab = page;
        } else {
            const uintptr_t frame_paddr = page_to_paddr(page);
            page->release_slab_to_allocated();
            m_stats.slab_pages--;
            free_frame(frame_paddr);
        }
    }
}

void* KernelHeap::compound_alloc_locked(size_t effective_size) noexcept {
    if (effective_size > (SIZE_MAX - (Arch::AArch64::PAGE_SIZE - 1U))) {
        return nullptr;
    }

    const size_t num_pages =
        (effective_size + Arch::AArch64::PAGE_SIZE - 1U) >> Arch::AArch64::PAGE_SHIFT;
    if (num_pages == 0U || num_pages > UINT32_MAX) {
        return nullptr;
    }

    const uintptr_t paddr = alloc_frames(num_pages);
    if (paddr == 0U) {
        return nullptr;
    }

    Page* const page = page_from_paddr(paddr);
    if (page == nullptr) {
        free_frames(paddr, num_pages);
        return nullptr;
    }

    page->configure_compound(num_pages);

    m_stats.compound_pages += num_pages;
    m_stats.active_bytes += num_pages * Arch::AArch64::PAGE_SIZE;

    return reinterpret_cast<void*>(Kernel::MM::phys_to_virt(paddr));
}

void KernelHeap::compound_free_locked(Page* page, uintptr_t paddr) noexcept {
    if ((paddr & (Arch::AArch64::PAGE_SIZE - 1U)) != 0U || page->compound_page_count() == 0U) {
        heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
    }

    const size_t num_pages = page->compound_page_count();
    page->release_compound_to_allocated();

    m_stats.compound_pages -= num_pages;
    m_stats.active_bytes -= num_pages * Arch::AArch64::PAGE_SIZE;

    free_frames(paddr, num_pages);
}

void KernelHeap::init() noexcept {
    const HeapLockGuard lock;

    for (size_t i = 0U; i < NUM_SLAB_CLASSES; ++i) {
        const size_t slot_size = MIN_SLAB_SIZE << i;
        m_caches[i] = SlabCache{
            .class_idx = static_cast<uint16_t>(i),
            .pad = 0U,
            .slot_size = slot_size,
            .slots_per_page = Arch::AArch64::PAGE_SIZE / slot_size,
            .partial_slabs = nullptr,
            .full_slabs = nullptr,
            .reserve_slab = nullptr,
        };
    }

    m_stats = HeapStats{};
}

void* KernelHeap::alloc(size_t size, size_t align) noexcept {
    const size_t raw_align = (align == 0U) ? MIN_HEAP_ALIGN : align;
    if (!is_power_of_two(raw_align) || raw_align > Arch::AArch64::PAGE_SIZE) {
        return nullptr;
    }

    const size_t effective_align = (raw_align < MIN_HEAP_ALIGN) ? MIN_HEAP_ALIGN : raw_align;
    const size_t promoted_size = (size == 0U) ? 1U : size;
    const size_t effective_size =
        (promoted_size < effective_align) ? effective_align : promoted_size;

    const HeapLockGuard lock;

    if (effective_size <= MAX_SLAB_SIZE) {
        const uint16_t class_idx = size_to_class_index(effective_size);
        return slab_alloc_locked(m_caches[class_idx]);
    }

    return compound_alloc_locked(effective_size);
}

void* KernelHeap::zalloc(size_t size, size_t align) noexcept {
    void* const ptr = alloc(size, align);
    if (ptr != nullptr) {
        const size_t zero_len = (size == 0U) ? 1U : size;
        __builtin_memset(ptr, 0, zero_len);
    }
    return ptr;
}

void* KernelHeap::realloc(void* ptr, size_t new_size, size_t align) noexcept {
    if (ptr == nullptr) {
        return alloc(new_size, align);
    }
    if (new_size == 0U) {
        free(ptr);
        return nullptr;
    }

    const size_t raw_align = (align == 0U) ? MIN_HEAP_ALIGN : align;
    if (!is_power_of_two(raw_align) || raw_align > Arch::AArch64::PAGE_SIZE) {
        return nullptr;
    }
    const size_t effective_align = (raw_align < MIN_HEAP_ALIGN) ? MIN_HEAP_ALIGN : raw_align;

    size_t old_capacity = 0U;
    bool alignment_compatible = false;

    {
        const HeapLockGuard lock;

        const uintptr_t paddr = Kernel::MM::virt_to_phys(reinterpret_cast<uintptr_t>(ptr));
        Page* const page = page_from_paddr(paddr & ~(Arch::AArch64::PAGE_SIZE - 1U));
        if (page == nullptr) {
            heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
        }

        if (page->flags() == PageFlags::Slab) {
            if (page->slab_class_index() >= NUM_SLAB_CLASSES || page->slab_ref_count() == 0U) {
                heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
            }
            old_capacity = MIN_SLAB_SIZE << page->slab_class_index();
            if ((paddr & (old_capacity - 1U)) != 0U) {
                heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
            }
            alignment_compatible =
                (effective_align <= old_capacity) && ((paddr & (effective_align - 1U)) == 0U);
        } else if (page->flags() == PageFlags::Compound) {
            if ((paddr & (Arch::AArch64::PAGE_SIZE - 1U)) != 0U ||
                page->compound_page_count() == 0U) {
                heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
            }
            old_capacity =
                static_cast<size_t>(page->compound_page_count()) * Arch::AArch64::PAGE_SIZE;
            alignment_compatible = ((paddr & (effective_align - 1U)) == 0U);
        } else {
            heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
        }
    }

    if (alignment_compatible && new_size <= old_capacity && new_size > (old_capacity / 2U)) {
        return ptr;
    }

    void* const new_ptr = alloc(new_size, align);
    if (new_ptr == nullptr) {
        return nullptr;
    }

    const size_t copy_bytes = (old_capacity < new_size) ? old_capacity : new_size;
    __builtin_memcpy(new_ptr, ptr, copy_bytes);
    free(ptr);
    return new_ptr;
}

void KernelHeap::free(void* ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }

    const HeapLockGuard lock;

    const uintptr_t paddr = Kernel::MM::virt_to_phys(reinterpret_cast<uintptr_t>(ptr));
    Page* const page = page_from_paddr(paddr & ~(Arch::AArch64::PAGE_SIZE - 1U));
    if (page == nullptr) {
        heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
    }

    if (page->flags() == PageFlags::Slab) {
        slab_free_locked(page, paddr, ptr);
    } else if (page->flags() == PageFlags::Compound) {
        compound_free_locked(page, paddr);
    } else {
        heap_panic("\r\n[PANIC] Invalid or double kfree()\r\n");
    }
}

HeapStats KernelHeap::stats() const noexcept {
    const HeapLockGuard lock;
    return m_stats;
}

bool run_concurrent_refcount_test() noexcept {
    constexpr uint32_t OPS_PER_WORKER = 500000U; // 2 * 500,000 = 1,000,000 parallel get/put pairs
    const size_t free_before = pfa_free_pages_count();

    const uintptr_t shared_frame = alloc_frame();
    if (shared_frame == 0U || page_ref_count(shared_frame) != 1U) {
        return false;
    }

    // Acquire one extra reference per concurrent worker (ref_count == 3)
    page_get(shared_frame);
    page_get(shared_frame);
    if (page_ref_count(shared_frame) != 3U) {
        free_frame(shared_frame);
        return false;
    }

    RefcountStressWorkerArgs worker_a_args{
        .shared_paddr = shared_frame,
        .iterations = OPS_PER_WORKER,
        .completed_iterations = 0U,
    };
    RefcountStressWorkerArgs worker_b_args{
        .shared_paddr = shared_frame,
        .iterations = OPS_PER_WORKER,
        .completed_iterations = 0U,
    };

#ifdef HOST_TEST
    std::thread t1(refcount_stress_worker_entry, reinterpret_cast<uintptr_t>(&worker_a_args));
    std::thread t2(refcount_stress_worker_entry, reinterpret_cast<uintptr_t>(&worker_b_args));
    t1.join();
    t2.join();
#else
    auto* const thread_a = Kernel::Process::Process::create(0U);
    if (thread_a == nullptr) {
        page_put(shared_frame);
        page_put(shared_frame);
        page_put(shared_frame);
        return false;
    }

    auto* const thread_b = Kernel::Process::Process::create(0U);
    if (thread_b == nullptr) {
        Kernel::Process::Process::destroy(*thread_a);
        page_put(shared_frame);
        page_put(shared_frame);
        page_put(shared_frame);
        return false;
    }

    HAL::Test::init_kernel_thread_context(thread_a->kctx(), thread_a->kstack_top(),
                                          &refcount_stress_worker_entry,
                                          reinterpret_cast<uintptr_t>(&worker_a_args));
    thread_a->set_kctx_initialized(true);

    HAL::Test::init_kernel_thread_context(thread_b->kctx(), thread_b->kstack_top(),
                                          &refcount_stress_worker_entry,
                                          reinterpret_cast<uintptr_t>(&worker_b_args));
    thread_b->set_kctx_initialized(true);

    Kernel::Scheduler::enqueue(thread_a);
    Kernel::Scheduler::enqueue(thread_b);
    Kernel::Scheduler::run_until_idle();

    Kernel::Process::Process::destroy(*thread_a);
    Kernel::Process::Process::destroy(*thread_b);
#endif

    const bool workers_ok =
        (__atomic_load_n(&worker_a_args.completed_iterations, __ATOMIC_ACQUIRE) ==
         OPS_PER_WORKER) &&
        (__atomic_load_n(&worker_b_args.completed_iterations, __ATOMIC_ACQUIRE) == OPS_PER_WORKER);

    // Frame must still be held by coordinator's single reference (ref_count == 1)
    const bool still_held =
        (page_ref_count(shared_frame) == 1U) && pfa_is_page_allocated(shared_frame);

    // Final release must reclaim the frame cleanly without a double-free panic
    page_put(shared_frame);

    const bool reclaimed_cleanly = (page_ref_count(shared_frame) == 0U) &&
                                   !pfa_is_page_allocated(shared_frame) &&
                                   (pfa_free_pages_count() == free_before);

    return workers_ok && still_held && reclaimed_cleanly;
}

} // namespace Kernel::MM
