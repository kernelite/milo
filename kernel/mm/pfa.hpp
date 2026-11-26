#pragma once

#include "boot/fdt.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::MM {

enum class PageFlags : uint8_t {
    Free = 0U,
    Allocated = 1U << 0U,
    Reserved = 1U << 1U,
    Slab = 1U << 2U,
    Compound = 1U << 3U,
};

constexpr PageFlags operator|(PageFlags lhs, PageFlags rhs) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<PageFlags>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}

constexpr PageFlags operator&(PageFlags lhs, PageFlags rhs) noexcept {
    return static_cast<PageFlags>(static_cast<uint8_t>(lhs) & static_cast<uint8_t>(rhs));
}

constexpr PageFlags operator^(PageFlags lhs, PageFlags rhs) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<PageFlags>(static_cast<uint8_t>(lhs) ^ static_cast<uint8_t>(rhs));
}

constexpr PageFlags operator~(PageFlags flags) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<PageFlags>(static_cast<uint8_t>(~static_cast<uint8_t>(flags)));
}

constexpr PageFlags& operator|=(PageFlags& lhs, PageFlags rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

constexpr PageFlags& operator&=(PageFlags& lhs, PageFlags rhs) noexcept {
    lhs = lhs & rhs;
    return lhs;
}

constexpr bool has_page_flag(PageFlags flags, PageFlags test) noexcept {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(test)) != 0U;
}

class PageFrameAllocator;
class KernelHeap;

class Page final {
  public:
    constexpr Page() noexcept = default;

    [[nodiscard]] uint32_t ref_count() const noexcept {
        return __atomic_load_n(&m_ref_count, __ATOMIC_ACQUIRE);
    }

    [[nodiscard]] bool has_shared_refs() const noexcept { return ref_count() > 1U; }

    [[nodiscard]] PageFlags flags() const noexcept {
        const auto raw =
            __atomic_load_n(reinterpret_cast<const uint8_t*>(&m_flags), __ATOMIC_ACQUIRE);
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        return static_cast<PageFlags>(raw);
    }

    [[nodiscard]] bool is_reserved() const noexcept {
        return has_page_flag(flags(), PageFlags::Reserved);
    }

    [[nodiscard]] bool is_free() const noexcept { return flags() == PageFlags::Free; }

    void try_retain() noexcept {
        if (is_reserved()) {
            return;
        }
        uint32_t current = __atomic_load_n(&m_ref_count, __ATOMIC_RELAXED);
        while (current > 0U && current < UINT32_MAX) {
            if (__atomic_compare_exchange_n(&m_ref_count, &current, current + 1U, false,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                return;
            }
        }
    }

    [[nodiscard]] bool release_ref() noexcept {
        if (is_reserved()) {
            return false;
        }
        uint32_t current = __atomic_load_n(&m_ref_count, __ATOMIC_RELAXED);
        while (true) {
            if (current == 0U) {
                return false;
            }
            if (__atomic_compare_exchange_n(&m_ref_count, &current, current - 1U, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                if (current != 1U) {
                    return false;
                }
                break;
            }
        }
        m_order = 0U;
        m_slab_cache = nullptr;
        m_next = nullptr;
        m_prev = nullptr;
        m_freelist = nullptr;
        store_flags(PageFlags::Free);
        return true;
    }

    [[nodiscard]] bool try_claim_free() noexcept {
        auto expected = static_cast<uint8_t>(PageFlags::Free);
        const auto desired = static_cast<uint8_t>(PageFlags::Allocated);
        if (!__atomic_compare_exchange_n(reinterpret_cast<uint8_t*>(&m_flags), &expected, desired,
                                         false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return false;
        }
        m_order = 0U;
        m_slab_cache = nullptr;
        m_next = nullptr;
        m_prev = nullptr;
        m_freelist = nullptr;
        __atomic_store_n(&m_ref_count, 1U, __ATOMIC_RELEASE);
        return true;
    }

    [[nodiscard]] constexpr uint16_t slab_class_index() const noexcept { return m_order; }
    [[nodiscard]] constexpr void* slab_cache() const noexcept { return m_slab_cache; }
    [[nodiscard]] constexpr uint32_t slab_ref_count() const noexcept { return m_ref_count; }
    [[nodiscard]] constexpr uint32_t compound_page_count() const noexcept { return m_ref_count; }
    [[nodiscard]] constexpr void* freelist_head() const noexcept { return m_freelist; }
    constexpr void set_freelist_head(void* head) noexcept { m_freelist = head; }
    constexpr void increment_slab_ref() noexcept { ++m_ref_count; }
    constexpr void decrement_slab_ref() noexcept {
        if (m_ref_count > 0U) {
            --m_ref_count;
        }
    }

    [[nodiscard]] constexpr Page* list_next() const noexcept { return m_next; }
    [[nodiscard]] constexpr Page* list_prev() const noexcept { return m_prev; }
    constexpr void set_list_links(Page* prev_page, Page* next_page) noexcept {
        m_prev = prev_page;
        m_next = next_page;
    }
    constexpr void clear_list_links() noexcept {
        m_prev = nullptr;
        m_next = nullptr;
    }

    void configure_slab(uint16_t class_idx, void* cache_ptr, void* freelist_head_ptr) noexcept {
        m_flags = PageFlags::Slab;
        m_order = class_idx;
        m_slab_cache = cache_ptr;
        m_ref_count = 0U;
        m_next = nullptr;
        m_prev = nullptr;
        m_freelist = freelist_head_ptr;
    }

    void release_slab_to_allocated() noexcept {
        m_flags = PageFlags::Allocated;
        m_ref_count = 1U;
        m_order = 0U;
        m_slab_cache = nullptr;
        m_next = nullptr;
        m_prev = nullptr;
        m_freelist = nullptr;
    }

    void configure_compound(size_t num_pages) noexcept {
        m_flags = PageFlags::Compound;
        m_ref_count = static_cast<uint32_t>(num_pages);
        m_order = (num_pages <= 0xFFFFU) ? static_cast<uint16_t>(num_pages) : 0xFFFFU;
        m_slab_cache = nullptr;
        m_next = nullptr;
        m_prev = nullptr;
        m_freelist = nullptr;
    }

    void release_compound_to_allocated() noexcept {
        m_flags = PageFlags::Allocated;
        m_ref_count = 1U;
        m_order = 0U;
        m_slab_cache = nullptr;
        m_next = nullptr;
        m_prev = nullptr;
        m_freelist = nullptr;
    }

  private:
    friend class PageFrameAllocator;

    void store_flags(PageFlags new_flags) noexcept {
        __atomic_store_n(reinterpret_cast<uint8_t*>(&m_flags), static_cast<uint8_t>(new_flags),
                         __ATOMIC_RELEASE);
    }

    void mark_reserved() noexcept {
        m_ref_count = 1U;
        m_flags = PageFlags::Reserved;
        m_order = 0U;
        m_slab_cache = nullptr;
        m_next = nullptr;
        m_prev = nullptr;
        m_freelist = nullptr;
    }

    void mark_free_init() noexcept {
        m_ref_count = 0U;
        m_flags = PageFlags::Free;
        m_order = 0U;
        m_slab_cache = nullptr;
        m_next = nullptr;
        m_prev = nullptr;
        m_freelist = nullptr;
    }

    void rollback_claim() noexcept {
        __atomic_store_n(&m_ref_count, 0U, __ATOMIC_RELAXED);
        store_flags(PageFlags::Free);
    }

    uint32_t m_ref_count{0U};
    PageFlags m_flags{PageFlags::Free};
    uint16_t m_order{0U};
    void* m_slab_cache{nullptr};
    Page* m_next{nullptr};
    Page* m_prev{nullptr};
    void* m_freelist{nullptr};
};

class PageFrameAllocator final {
  public:
    constexpr PageFrameAllocator() noexcept = default;
    ~PageFrameAllocator() noexcept = default;
    PageFrameAllocator(const PageFrameAllocator&) = delete;
    PageFrameAllocator& operator=(const PageFrameAllocator&) = delete;
    PageFrameAllocator(PageFrameAllocator&&) = delete;
    PageFrameAllocator& operator=(PageFrameAllocator&&) = delete;

    [[nodiscard]] static PageFrameAllocator& instance() noexcept;

    void init(const Kernel::Boot::MemoryMap& mem_map, uintptr_t kernel_start, uintptr_t kernel_end,
              uintptr_t dtb_paddr, size_t dtb_size, uintptr_t initrd_start, uintptr_t initrd_end,
              const Kernel::Boot::ReserveMap* reserve_map = nullptr) noexcept;
    void on_mmu_enabled() noexcept;

    [[nodiscard]] uintptr_t alloc_frame() noexcept;
    [[nodiscard]] uintptr_t alloc_frames(size_t count) noexcept;
    void free_frame(uintptr_t paddr) noexcept;
    void free_frames(uintptr_t paddr, size_t count) noexcept;

    void page_get(uintptr_t paddr) const noexcept;
    void page_put(uintptr_t paddr) noexcept;
    [[nodiscard]] uint32_t page_ref_count(uintptr_t paddr) const noexcept;

    [[nodiscard]] Page* page_from_paddr(uintptr_t paddr) const noexcept;
    [[nodiscard]] uintptr_t page_to_paddr(const Page* page) const noexcept;

    [[nodiscard]] bool is_page_allocated(uintptr_t paddr) const noexcept;
    [[nodiscard]] size_t allocated_pages_count() const noexcept;
    [[nodiscard]] size_t free_pages_count() const noexcept;
    [[nodiscard]] size_t total_pages_count() const noexcept { return m_total_pages; }
    [[nodiscard]] uintptr_t metadata_paddr() const noexcept { return m_pages_paddr; }
    [[nodiscard]] size_t metadata_size() const noexcept { return m_pages_bytes; }

  private:
    void mark_range_reserved(uintptr_t start, uintptr_t end) noexcept;

    Page* m_pages{nullptr};
    uintptr_t m_pages_paddr{0U};
    size_t m_pages_bytes{0U};
    uintptr_t m_ram_start{0U};
    uintptr_t m_ram_end{0U};
    size_t m_total_pages{0U};
    size_t m_free_pages{0U};
    size_t m_search_hint{0U};
};

inline void pfa_init(const Kernel::Boot::MemoryMap& mem_map, uintptr_t kernel_start,
                     uintptr_t kernel_end, uintptr_t dtb_paddr, size_t dtb_size,
                     uintptr_t initrd_start, uintptr_t initrd_end,
                     const Kernel::Boot::ReserveMap* reserve_map = nullptr) noexcept {
    PageFrameAllocator::instance().init(mem_map, kernel_start, kernel_end, dtb_paddr, dtb_size,
                                        initrd_start, initrd_end, reserve_map);
}

inline void pfa_on_mmu_enabled() noexcept {
    PageFrameAllocator::instance().on_mmu_enabled();
}

[[nodiscard]] inline uintptr_t alloc_frame() noexcept {
    return PageFrameAllocator::instance().alloc_frame();
}

[[nodiscard]] inline uintptr_t alloc_frames(size_t count) noexcept {
    return PageFrameAllocator::instance().alloc_frames(count);
}

inline void free_frame(uintptr_t paddr) noexcept {
    PageFrameAllocator::instance().free_frame(paddr);
}

inline void free_frames(uintptr_t paddr, size_t count) noexcept {
    PageFrameAllocator::instance().free_frames(paddr, count);
}

inline void page_get(uintptr_t paddr) noexcept {
    PageFrameAllocator::instance().page_get(paddr);
}

inline void page_put(uintptr_t paddr) noexcept {
    PageFrameAllocator::instance().page_put(paddr);
}

[[nodiscard]] inline uint32_t page_ref_count(uintptr_t paddr) noexcept {
    return PageFrameAllocator::instance().page_ref_count(paddr);
}

[[nodiscard]] inline Page* page_from_paddr(uintptr_t paddr) noexcept {
    return PageFrameAllocator::instance().page_from_paddr(paddr);
}

[[nodiscard]] inline uintptr_t page_to_paddr(const Page* page) noexcept {
    return PageFrameAllocator::instance().page_to_paddr(page);
}

[[nodiscard]] inline bool pfa_is_page_allocated(uintptr_t paddr) noexcept {
    return PageFrameAllocator::instance().is_page_allocated(paddr);
}

[[nodiscard]] inline size_t pfa_allocated_pages_count() noexcept {
    return PageFrameAllocator::instance().allocated_pages_count();
}

[[nodiscard]] inline size_t pfa_free_pages_count() noexcept {
    return PageFrameAllocator::instance().free_pages_count();
}

} // namespace Kernel::MM
