#pragma once

#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "loader/elf64.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::MM {

inline uintptr_t phys_to_virt(uintptr_t paddr) noexcept {
    return paddr + HAL::get_platform_info().higher_half_offset;
}

inline uintptr_t virt_to_phys(uintptr_t vaddr) noexcept {
    return vaddr - HAL::get_platform_info().higher_half_offset;
}

inline uintptr_t user_addr_space_limit() noexcept {
    return HAL::get_platform_info().user_va_limit;
}

enum class AddressSpaceError {
    Success = 0,
    OutOfMemory,
    InvalidVirtualAddress,
    InvalidPhysicalAddress,
    PageAlreadyMapped,
    NotMapped,
    RootAllocationFailed
};

const char* address_space_error_str(AddressSpaceError error) noexcept;

class AddressSpace {
  public:
    constexpr AddressSpace() noexcept : m_root_paddr(0U) {}
    explicit constexpr AddressSpace(uintptr_t root_paddr) noexcept : m_root_paddr(root_paddr) {}

    ~AddressSpace() noexcept { destroy(); }

    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;

    AddressSpace(AddressSpace&& other) noexcept : m_root_paddr(other.m_root_paddr) {
        other.m_root_paddr = 0U;
    }

    AddressSpace& operator=(AddressSpace&& other) noexcept {
        if (this != &other) {
            destroy();
            m_root_paddr = other.m_root_paddr;
            other.m_root_paddr = 0U;
        }
        return *this;
    }

    static Result<AddressSpace, AddressSpaceError> create() noexcept;

    [[nodiscard]] bool is_valid() const noexcept { return m_root_paddr != 0U; }

    [[nodiscard]] uintptr_t root_paddr() const noexcept { return m_root_paddr; }

    bool map_user_page(uintptr_t vaddr, uintptr_t paddr, HAL::PageFlags flags) const noexcept;

    bool unmap_user_page(uintptr_t vaddr) const noexcept;

    bool query_user_page(uintptr_t vaddr, uintptr_t& out_paddr,
                         HAL::PageFlags& out_flags) const noexcept;

    static bool query_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t& out_paddr,
                                HAL::PageFlags& out_flags) noexcept;

    void activate() const noexcept;

    void destroy() noexcept;

    static void activate_root(uintptr_t root_paddr) noexcept;

    static uintptr_t current_root_paddr() noexcept;

  private:
    uintptr_t m_root_paddr{0U};
};

// Safe freestanding user-memory transfer helpers.
// Returns 0 on success, or -14 (-EFAULT) if any byte falls outside valid EL0 mappings.
int64_t copy_from_user(uintptr_t root_paddr, void* dst, uintptr_t user_src, size_t len) noexcept;
int64_t copy_to_user(uintptr_t root_paddr, uintptr_t user_dst, const void* src,
                     size_t len) noexcept;

inline int64_t copy_from_user(const AddressSpace& adrsp, void* dst, uintptr_t user_src,
                              size_t len) noexcept {
    return copy_from_user(adrsp.root_paddr(), dst, user_src, len);
}

inline int64_t copy_to_user(const AddressSpace& adrsp, uintptr_t user_dst, const void* src,
                            size_t len) noexcept {
    return copy_to_user(adrsp.root_paddr(), user_dst, src, len);
}

inline int64_t copy_from_user(void* dst, uintptr_t user_src, size_t len) noexcept {
    return copy_from_user(AddressSpace::current_root_paddr(), dst, user_src, len);
}

inline int64_t copy_to_user(uintptr_t user_dst, const void* src, size_t len) noexcept {
    return copy_to_user(AddressSpace::current_root_paddr(), user_dst, src, len);
}

} // namespace Kernel::MM