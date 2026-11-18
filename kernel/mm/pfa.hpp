#pragma once

#include "boot/fdt.hpp"

#include <cstddef>
#include <cstdint>

enum class PageFlags : uint16_t {
    Free = 0U,
    Allocated = 1U << 0U,
    Reserved = 1U << 1U,
    Slab = 1U << 2U,
    Compound = 1U << 3U,
};

constexpr PageFlags operator|(PageFlags lhs, PageFlags rhs) noexcept {
    return static_cast<PageFlags>(static_cast<uint16_t>(lhs) | static_cast<uint16_t>(rhs));
}

constexpr PageFlags operator&(PageFlags lhs, PageFlags rhs) noexcept {
    return static_cast<PageFlags>(static_cast<uint16_t>(lhs) & static_cast<uint16_t>(rhs));
}

constexpr PageFlags& operator|=(PageFlags& lhs, PageFlags rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

constexpr PageFlags& operator&=(PageFlags& lhs, PageFlags rhs) noexcept {
    lhs = lhs & rhs;
    return lhs;
}

constexpr bool has_page_flag(PageFlags value, PageFlags flag) noexcept {
    return (static_cast<uint16_t>(value) & static_cast<uint16_t>(flag)) != 0U;
}

struct Page {
    uint32_t ref_count{0U};
    PageFlags flags{PageFlags::Free};
    uint16_t order{0U};        // Buddy order or slab size-class index
    void* slab_cache{nullptr}; // Owning SlabCache for O(1) kfree()
};

static_assert(sizeof(Page) == 16U, "struct Page must be exactly 16 bytes");

void pfa_init(const FDT::MemoryMap& mem_map, uintptr_t kernel_start, uintptr_t kernel_end,
              uintptr_t dtb_paddr = 0U, size_t dtb_size = 0U, uintptr_t initrd_start = 0U,
              uintptr_t initrd_end = 0U, const FDT::ReserveMap* reserve_map = nullptr) noexcept;

void pfa_init(uintptr_t ram_start, size_t ram_size, uintptr_t kernel_start = 0U,
              uintptr_t kernel_end = 0U, uintptr_t initrd_start = 0U,
              uintptr_t initrd_end = 0U) noexcept;

void pfa_on_mmu_enabled() noexcept;

Page* page_from_paddr(uintptr_t paddr) noexcept;
uintptr_t page_to_paddr(const Page* page) noexcept;
void page_get(uintptr_t paddr) noexcept;
void page_put(uintptr_t paddr) noexcept;

uintptr_t alloc_frame() noexcept;
uintptr_t alloc_frames(size_t count) noexcept;
void free_frame(uintptr_t paddr) noexcept;
void free_frames(uintptr_t paddr, size_t count) noexcept;

bool pfa_is_page_allocated(uintptr_t paddr) noexcept;
size_t pfa_free_pages_count() noexcept;
size_t pfa_allocated_pages_count() noexcept;
size_t pfa_total_pages_count() noexcept;
uintptr_t pfa_metadata_paddr() noexcept;
size_t pfa_metadata_size() noexcept;