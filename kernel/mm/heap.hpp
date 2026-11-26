#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::MM {

class Page;

struct HeapStats {
    size_t active_slab_objects{0U};
    size_t slab_pages{0U};
    size_t compound_pages{0U};
    size_t active_bytes{0U};
};

class KernelHeap final {
  public:
    static constexpr size_t NUM_SLAB_CLASSES = 8U;
    static constexpr size_t MIN_SLAB_SIZE = 16U;
    static constexpr size_t MAX_SLAB_SIZE = 2048U;
    static constexpr size_t MIN_HEAP_ALIGN = 16U;

    constexpr KernelHeap() noexcept = default;
    ~KernelHeap() noexcept = default;
    KernelHeap(const KernelHeap&) = delete;
    KernelHeap& operator=(const KernelHeap&) = delete;
    KernelHeap(KernelHeap&&) = delete;
    KernelHeap& operator=(KernelHeap&&) = delete;

    [[nodiscard]] static KernelHeap& instance() noexcept;

    void init() noexcept;
    [[nodiscard]] void* alloc(size_t size, size_t align = MIN_HEAP_ALIGN) noexcept;
    [[nodiscard]] void* zalloc(size_t size, size_t align = MIN_HEAP_ALIGN) noexcept;
    [[nodiscard]] void* realloc(void* ptr, size_t new_size, size_t align = MIN_HEAP_ALIGN) noexcept;
    void free(void* ptr) noexcept;
    [[nodiscard]] HeapStats stats() const noexcept;

  private:
    struct SlabCache {
        uint16_t class_idx{0U};
        uint16_t pad{0U};
        size_t slot_size{0U};
        size_t slots_per_page{0U};
        Page* partial_slabs{nullptr};
        Page* full_slabs{nullptr};
        Page* reserve_slab{nullptr};
    };

    static void list_push_front(Page*& head, Page* page) noexcept;
    static void list_remove(Page*& head, Page* page) noexcept;

    Page* create_slab_page(SlabCache& cache) noexcept;
    void* slab_alloc_locked(SlabCache& cache) noexcept;
    void slab_free_locked(Page* page, uintptr_t paddr, void* ptr) noexcept;
    void* compound_alloc_locked(size_t effective_size) noexcept;
    void compound_free_locked(Page* page, uintptr_t paddr) noexcept;

    SlabCache m_caches[NUM_SLAB_CLASSES]{
        {.class_idx = 0U, .slot_size = 16U, .slots_per_page = 256U},
        {.class_idx = 1U, .slot_size = 32U, .slots_per_page = 128U},
        {.class_idx = 2U, .slot_size = 64U, .slots_per_page = 64U},
        {.class_idx = 3U, .slot_size = 128U, .slots_per_page = 32U},
        {.class_idx = 4U, .slot_size = 256U, .slots_per_page = 16U},
        {.class_idx = 5U, .slot_size = 512U, .slots_per_page = 8U},
        {.class_idx = 6U, .slot_size = 1024U, .slots_per_page = 4U},
        {.class_idx = 7U, .slot_size = 2048U, .slots_per_page = 2U},
    };
    HeapStats m_stats{};
};

inline void heap_init() noexcept {
    KernelHeap::instance().init();
}

[[nodiscard]] inline void* kmalloc(size_t size,
                                   size_t align = KernelHeap::MIN_HEAP_ALIGN) noexcept {
    return KernelHeap::instance().alloc(size, align);
}

[[nodiscard]] inline void* kzalloc(size_t size,
                                   size_t align = KernelHeap::MIN_HEAP_ALIGN) noexcept {
    return KernelHeap::instance().zalloc(size, align);
}

[[nodiscard]] inline void* krealloc(void* ptr, size_t new_size,
                                    size_t align = KernelHeap::MIN_HEAP_ALIGN) noexcept {
    return KernelHeap::instance().realloc(ptr, new_size, align);
}

inline void kfree(void* ptr) noexcept {
    KernelHeap::instance().free(ptr);
}

[[nodiscard]] inline HeapStats heap_get_stats() noexcept {
    return KernelHeap::instance().stats();
}

[[nodiscard]] bool run_concurrent_refcount_test() noexcept;

} // namespace Kernel::MM
