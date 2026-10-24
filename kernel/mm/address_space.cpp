#include "mm/address_space.hpp"

#include "hal/mmu.hpp"

namespace Kernel::MM {

Result<AddressSpace, AddressSpaceError> AddressSpace::create() noexcept {
    const uintptr_t root_phys = HAL::AddressSpaceBackend::create_root();
    if (root_phys == 0U) {
        return AddressSpaceError::RootAllocationFailed;
    }
    return AddressSpace(root_phys);
}

bool AddressSpace::map_user_page(uintptr_t vaddr, uintptr_t paddr,
                                 HAL::PageFlags flags) const noexcept {
    return HAL::AddressSpaceBackend::map_user_page(m_root_paddr, vaddr, paddr, flags);
}

bool AddressSpace::unmap_user_page(uintptr_t vaddr) const noexcept {
    return HAL::AddressSpaceBackend::unmap_user_page(m_root_paddr, vaddr);
}

void AddressSpace::activate() const noexcept {
    HAL::AddressSpaceBackend::activate_root(m_root_paddr);
}

void AddressSpace::activate_root(uintptr_t root_paddr) noexcept {
    HAL::AddressSpaceBackend::activate_root(root_paddr);
}

uintptr_t AddressSpace::current_root_paddr() noexcept {
    return HAL::AddressSpaceBackend::current_root_paddr();
}

void AddressSpace::destroy() noexcept {
    if (m_root_paddr != 0U) {
        HAL::AddressSpaceBackend::destroy_root(m_root_paddr);
        m_root_paddr = 0U;
    }
}

} // namespace Kernel::MM