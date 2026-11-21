#pragma once

#include "boot/fdt.hpp"

#include <cstddef>
#include <cstdint>

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

struct Page {
    uint32_t ref_count{0U};
    PageFlags flags{PageFlags::Free};
    uint8_t _pad{0U};
    uint16_t order{0U};
    void* slab_cache{nullptr};
    Page* next{nullptr};
    Page* prev{nullptr};
    void* freelist{nullptr};
};

void pfa_init(const FDT::MemoryMap& mem_map, uintptr_t kernel_start, uintptr_t kernel_end,
              uintptr_t dtb_start, size_t dtb_size, uintptr_t initrd_start, uintptr_t initrd_end,
              const FDT::ReserveMap* reserve_map = nullptr) noexcept;

void pfa_on_mmu_enabled() noexcept;

[[nodiscard]] uintptr_t alloc_frame() noexcept;
[[nodiscard]] uintptr_t alloc_frames(size_t count) noexcept;
void free_frame(uintptr_t paddr) noexcept;
void free_frames(uintptr_t paddr, size_t count) noexcept;

void page_get(uintptr_t paddr) noexcept;
void page_put(uintptr_t paddr) noexcept;
[[nodiscard]] uint32_t page_ref_count(uintptr_t paddr) noexcept;

[[nodiscard]] Page* page_from_paddr(uintptr_t paddr) noexcept;
[[nodiscard]] uintptr_t page_to_paddr(const Page* page) noexcept;
void page_get(uintptr_t paddr) noexcept;
void page_put(uintptr_t paddr) noexcept;

[[nodiscard]] bool pfa_is_page_allocated(uintptr_t paddr) noexcept;
[[nodiscard]] size_t pfa_allocated_pages_count() noexcept;
[[nodiscard]] size_t pfa_free_pages_count() noexcept;