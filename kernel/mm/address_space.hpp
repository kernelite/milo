#pragma once

#include "hal/mmu.hpp"
#include "loader/elf64.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::MM {

constexpr uintptr_t HIGHER_HALF_OFFSET = 0xFFFF800000000000ULL;

constexpr uintptr_t phys_to_virt(uintptr_t paddr) noexcept {
    return paddr + HIGHER_HALF_OFFSET;
}

constexpr uintptr_t virt_to_phys(uintptr_t vaddr) noexcept {
    return vaddr - HIGHER_HALF_OFFSET;
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

    void activate() const noexcept;

    void destroy() noexcept;

    static void activate_root(uintptr_t root_paddr) noexcept;

    static uintptr_t current_root_paddr() noexcept;

  private:
    uintptr_t m_root_paddr{0U};
};

} // namespace Kernel::MM