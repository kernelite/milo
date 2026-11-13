#include "mm/address_space.hpp"

#include "hal/cpu.hpp"
#include "hal/mmu.hpp"

extern "C" void* memcpy(void* dest, const void* src, size_t count) noexcept;

namespace {

constexpr int64_t ERR_EFAULT = -14;
constexpr size_t USER_PAGE_SIZE = 4096U;
constexpr uintptr_t USER_PAGE_OFFSET_MASK = USER_PAGE_SIZE - 1U;

} // namespace

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

bool AddressSpace::query_user_page(uintptr_t vaddr, uintptr_t& out_paddr,
                                   HAL::PageFlags& out_flags) const noexcept {
    return HAL::AddressSpaceBackend::query_user_page(m_root_paddr, vaddr, out_paddr, out_flags);
}

bool AddressSpace::query_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t& out_paddr,
                                   HAL::PageFlags& out_flags) noexcept {
    return HAL::AddressSpaceBackend::query_user_page(root_paddr, vaddr, out_paddr, out_flags);
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

int64_t copy_from_user(uintptr_t root_paddr, void* dst, uintptr_t user_src, size_t len) noexcept {
    if (user_src >= USER_ADDR_SPACE_LIMIT) {
        return ERR_EFAULT;
    }
    if (len == 0U) {
        return 0;
    }
    if (root_paddr == 0U || dst == nullptr || len > (USER_ADDR_SPACE_LIMIT - user_src)) {
        return ERR_EFAULT;
    }

    auto* dst_bytes = static_cast<uint8_t*>(dst);
    uintptr_t curr_vaddr = user_src;
    size_t remaining = len;

    while (remaining > 0U) {
        uintptr_t paddr = 0U;
        HAL::PageFlags flags = HAL::PageFlags::None;
        if (!AddressSpace::query_user_page(root_paddr, curr_vaddr, paddr, flags)) {
            return ERR_EFAULT;
        }
        if (!HAL::has_flag(flags, HAL::PageFlags::User) ||
            !HAL::has_flag(flags, HAL::PageFlags::Read)) {
            return ERR_EFAULT;
        }

        const size_t page_offset = static_cast<size_t>(curr_vaddr & USER_PAGE_OFFSET_MASK);
        const size_t bytes_in_page = USER_PAGE_SIZE - page_offset;
        const size_t chunk = (remaining < bytes_in_page) ? remaining : bytes_in_page;

        const auto* const src_kva = reinterpret_cast<const uint8_t*>(phys_to_virt(paddr));
        memcpy(dst_bytes, src_kva, chunk);

        dst_bytes += chunk;
        curr_vaddr += chunk;
        remaining -= chunk;
    }

    return 0;
}

int64_t copy_to_user(uintptr_t root_paddr, uintptr_t user_dst, const void* src,
                     size_t len) noexcept {
    if (user_dst >= USER_ADDR_SPACE_LIMIT) {
        return ERR_EFAULT;
    }
    if (len == 0U) {
        return 0;
    }
    if (root_paddr == 0U || src == nullptr || len > (USER_ADDR_SPACE_LIMIT - user_dst)) {
        return ERR_EFAULT;
    }

    const auto* src_bytes = static_cast<const uint8_t*>(src);
    uintptr_t curr_vaddr = user_dst;
    size_t remaining = len;

    while (remaining > 0U) {
        uintptr_t paddr = 0U;
        HAL::PageFlags flags = HAL::PageFlags::None;
        if (!AddressSpace::query_user_page(root_paddr, curr_vaddr, paddr, flags)) {
            return ERR_EFAULT;
        }
        if (!HAL::has_flag(flags, HAL::PageFlags::User) ||
            !HAL::has_flag(flags, HAL::PageFlags::Write)) {
            return ERR_EFAULT;
        }

        const size_t page_offset = static_cast<size_t>(curr_vaddr & USER_PAGE_OFFSET_MASK);
        const size_t bytes_in_page = USER_PAGE_SIZE - page_offset;
        const size_t chunk = (remaining < bytes_in_page) ? remaining : bytes_in_page;

        auto* const dst_kva = reinterpret_cast<uint8_t*>(phys_to_virt(paddr));
        memcpy(dst_kva, src_bytes, chunk);

        src_bytes += chunk;
        curr_vaddr += chunk;
        remaining -= chunk;
    }

    return 0;
}

} // namespace Kernel::MM