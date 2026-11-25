#pragma once

#include <cstddef>
#include <cstdint>

namespace HAL {

enum class PageFlags : uint8_t {
    None = 0U,
    Read = 1U << 0U,
    Write = 1U << 1U,
    Execute = 1U << 2U,
    User = 1U << 3U,
    Device = 1U << 4U,
    Cacheable = 1U << 5U
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

constexpr bool has_flag(PageFlags flags, PageFlags test) noexcept {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0U;
}

struct MmuStatus {
    bool mmu_enabled;
    bool dcache_enabled;
    bool icache_enabled;
    uint64_t raw_control_reg;
};

class MemoryControl {
  public:
    constexpr MemoryControl() = default;
    virtual ~MemoryControl() = default;

    MemoryControl(const MemoryControl&) = delete;
    MemoryControl& operator=(const MemoryControl&) = delete;
    MemoryControl(MemoryControl&&) = delete;
    MemoryControl& operator=(MemoryControl&&) = delete;

    virtual bool map(uintptr_t virt_addr, uintptr_t phys_addr, PageFlags flags) = 0;
    virtual bool unmap(uintptr_t virt_addr) = 0;
    virtual void activate() = 0;
    virtual MmuStatus status() const noexcept = 0;
};

MemoryControl& get_mmu() noexcept;

struct AddressSpaceBackend {
    static uintptr_t create_root() noexcept;
    static bool clone_user_space(uintptr_t src_root, uintptr_t dst_root) noexcept;
    static bool map_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t paddr,
                              PageFlags flags) noexcept;
    static bool map_demand_zero(uintptr_t root_paddr, uintptr_t vaddr,
                                PageFlags expected_flags) noexcept;
    static bool resolve_cow_page(uintptr_t root_paddr, uintptr_t vaddr,
                                 uintptr_t& out_paddr) noexcept;
    static bool resolve_demand_zero_page(uintptr_t root_paddr, uintptr_t vaddr, bool require_write,
                                         uintptr_t& out_paddr) noexcept;
    static bool unmap_user_page(uintptr_t root_paddr, uintptr_t vaddr) noexcept;
    static bool protect_user_page(uintptr_t root_paddr, uintptr_t vaddr, PageFlags flags) noexcept;
    static bool query_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t& out_paddr,
                                PageFlags& out_flags) noexcept;
    static bool query_pte_flags(uintptr_t root_paddr, uintptr_t vaddr,
                                uint64_t& out_raw_pte) noexcept;
    static bool remap_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t paddr,
                                PageFlags flags) noexcept;
    static void activate_root(uintptr_t root_paddr) noexcept;
    static uintptr_t current_root_paddr() noexcept;
    static void destroy_root(uintptr_t root_paddr) noexcept;
};

} // namespace HAL