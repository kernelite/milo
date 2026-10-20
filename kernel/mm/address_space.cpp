#include "mm/address_space.hpp"

#include "../arch/memory_config.hpp"
#include "mm/pfa.hpp"

namespace {

constexpr size_t PAGE_ENTRY_COUNT = 512U;
constexpr uint64_t TABLE_ADDR_MASK = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t DESC_TYPE_TABLE = 0x3ULL;
constexpr uint64_t DESC_TYPE_PAGE = 0x3ULL;
constexpr uint64_t DESC_AF_BIT = 1ULL << 10U;
constexpr uint64_t DESC_PXN_BIT = 1ULL << 53U;
constexpr uint64_t DESC_UXN_BIT = 1ULL << 54U;

inline void flush_dcache_range(uintptr_t addr, size_t size) noexcept {
    constexpr uintptr_t line_size = 64U;
    const uintptr_t end = addr + size;
    addr &= ~(line_size - 1U);
    for (; addr < end; addr += line_size) {
        asm volatile("dc civac, %0" ::"r"(addr) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

uintptr_t allocate_zeroed_table() noexcept {
    const uintptr_t phys = alloc_frame();
    if (phys == 0U) {
        return 0U;
    }

    auto* const table = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(phys));
    for (size_t i = 0U; i < PAGE_ENTRY_COUNT; ++i) {
        table[i] = 0ULL;
    }
    flush_dcache_range(reinterpret_cast<uintptr_t>(table), Arch::PAGE_SIZE);
    return phys;
}

uintptr_t get_boot_ttbr0() noexcept {
    static uintptr_t boot_ttbr0 = 0U;
    if (boot_ttbr0 == 0U) {
        asm volatile("mrs %0, ttbr0_el1" : "=r"(boot_ttbr0));
    }
    return boot_ttbr0;
}

} // namespace

namespace Kernel::MM {

const char* address_space_error_str(AddressSpaceError error) noexcept {
    switch (error) {
    case AddressSpaceError::Success:
        return "Success";
    case AddressSpaceError::OutOfMemory:
        return "OutOfMemory";
    case AddressSpaceError::InvalidVirtualAddress:
        return "InvalidVirtualAddress";
    case AddressSpaceError::InvalidPhysicalAddress:
        return "InvalidPhysicalAddress";
    case AddressSpaceError::PageAlreadyMapped:
        return "PageAlreadyMapped";
    case AddressSpaceError::NotMapped:
        return "NotMapped";
    case AddressSpaceError::RootAllocationFailed:
        return "RootAllocationFailed";
    default:
        return "UnknownError";
    }
}

Result<AddressSpace, AddressSpaceError> AddressSpace::create() noexcept {
    const uintptr_t root_phys = allocate_zeroed_table();
    if (root_phys == 0U) {
        return AddressSpaceError::RootAllocationFailed;
    }

    // Capture boot TTBR0_EL1 root table on first invocation
    const uintptr_t boot_ttbr0 = get_boot_ttbr0();
    if (boot_ttbr0 != 0U) {
        auto* const new_l0 = reinterpret_cast<uint64_t*>(phys_to_virt(root_phys));
        const auto* const k_l0 = reinterpret_cast<const uint64_t*>(phys_to_virt(boot_ttbr0));

        // Allocate a dedicated L1 table for this AddressSpace so user mappings in L1[0]
        // do not collide with or modify the kernel's boot L1 table.
        const uintptr_t l1_phys = allocate_zeroed_table();
        if (l1_phys == 0U) {
            free_frame(root_phys);
            return AddressSpaceError::RootAllocationFailed;
        }

        new_l0[0] = (l1_phys & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&new_l0[0]), sizeof(uint64_t));

        auto* const new_l1 = reinterpret_cast<uint64_t*>(phys_to_virt(l1_phys));
        const auto* const k_l1 =
            reinterpret_cast<const uint64_t*>(phys_to_virt(k_l0[0] & TABLE_ADDR_MASK));

        // Copy kernel RAM mappings (L1[1] covers 0x40000000..0x7FFFFFFF)
        // Descriptors have AP[1]=0 and UXN=1, completely isolating them from EL0 user space.
        for (size_t i = 1U; i < PAGE_ENTRY_COUNT; ++i) {
            new_l1[i] = k_l1[i];
        }

        // Allocate dedicated L2 table for user space (L1[0] covers 0x00000000..0x3FFFFFFF)
        const uintptr_t l2_phys = allocate_zeroed_table();
        if (l2_phys != 0U) {
            new_l1[0] = (l2_phys & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
            auto* const new_l2 = reinterpret_cast<uint64_t*>(phys_to_virt(l2_phys));
            const auto* const k_l2 =
                reinterpret_cast<const uint64_t*>(phys_to_virt(k_l1[0] & TABLE_ADDR_MASK));

            // Mirror UART MMIO (0x09000000 >> 21 = 45) so kernel syscalls can write to console
            new_l2[45] = k_l2[45];
            flush_dcache_range(reinterpret_cast<uintptr_t>(new_l2), Arch::PAGE_SIZE);
        }
        flush_dcache_range(reinterpret_cast<uintptr_t>(new_l1), Arch::PAGE_SIZE);
    }

    return AddressSpace(root_phys);
}

bool AddressSpace::map_user_page(uintptr_t vaddr, uintptr_t paddr,
                                 HAL::PageFlags flags) noexcept {
    if (m_root_paddr == 0U) {
        return false;
    }

    if ((vaddr & (Arch::PAGE_SIZE - 1U)) != 0U || (paddr & (Arch::PAGE_SIZE - 1U)) != 0U) {
        return false;
    }

    // Enforce lower-half user address space boundary (< 1GB, preventing collision with kernel in L1[1])
    if (vaddr >= 0x40000000ULL) {
        return false;
    }

    const size_t l0_idx = (vaddr >> 39U) & 0x1FFU;
    const size_t l1_idx = (vaddr >> 30U) & 0x1FFU;
    const size_t l2_idx = (vaddr >> 21U) & 0x1FFU;
    const size_t l3_idx = (vaddr >> 12U) & 0x1FFU;

    auto* const l0_table = reinterpret_cast<uint64_t*>(phys_to_virt(m_root_paddr));

    // L0 -> L1
    if ((l0_table[l0_idx] & 1U) == 0U) {
        const uintptr_t l1_paddr = allocate_zeroed_table();
        if (l1_paddr == 0U) {
            return false;
        }
        l0_table[l0_idx] = (l1_paddr & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&l0_table[l0_idx]), sizeof(uint64_t));
    }
    auto* const l1_table =
        reinterpret_cast<uint64_t*>(phys_to_virt(l0_table[l0_idx] & TABLE_ADDR_MASK));

    // L1 -> L2
    if ((l1_table[l1_idx] & 1U) == 0U) {
        const uintptr_t l2_paddr = allocate_zeroed_table();
        if (l2_paddr == 0U) {
            return false;
        }
        l1_table[l1_idx] = (l2_paddr & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&l1_table[l1_idx]), sizeof(uint64_t));
    }
    auto* const l2_table =
        reinterpret_cast<uint64_t*>(phys_to_virt(l1_table[l1_idx] & TABLE_ADDR_MASK));

    // L2 -> L3
    if ((l2_table[l2_idx] & 1U) == 0U) {
        const uintptr_t l3_paddr = allocate_zeroed_table();
        if (l3_paddr == 0U) {
            return false;
        }
        l2_table[l2_idx] = (l3_paddr & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&l2_table[l2_idx]), sizeof(uint64_t));
    }
    auto* const l3_table =
        reinterpret_cast<uint64_t*>(phys_to_virt(l2_table[l2_idx] & TABLE_ADDR_MASK));

    // L3 Page Descriptor Construction
    uint64_t desc = (paddr & TABLE_ADDR_MASK) | DESC_TYPE_PAGE;
    desc |= DESC_AF_BIT;

    const bool is_write = has_flag(flags, HAL::PageFlags::Write);
    const bool is_user = has_flag(flags, HAL::PageFlags::User);
    const bool is_exec = has_flag(flags, HAL::PageFlags::Execute);
    const bool is_device = has_flag(flags, HAL::PageFlags::Device);

    const uint64_t attr_idx = is_device ? 0ULL : 1ULL;
    desc |= (attr_idx << 2U);

    uint64_t ap_bits = 0ULL;
    if (is_write) {
        ap_bits = is_user ? 0x1ULL : 0x0ULL;
    } else {
        ap_bits = is_user ? 0x3ULL : 0x2ULL;
    }
    desc |= (ap_bits << 6U);

    if (!is_device) {
        desc |= (3ULL << 8U); // Inner Shareable
    }

    if (!is_exec) {
        desc |= DESC_PXN_BIT | DESC_UXN_BIT;
    } else {
        desc |= DESC_PXN_BIT; // User code cannot execute in EL1 context
    }

    l3_table[l3_idx] = desc;
    flush_dcache_range(reinterpret_cast<uintptr_t>(&l3_table[l3_idx]), sizeof(uint64_t));
    return true;
}

bool AddressSpace::unmap_user_page(uintptr_t vaddr) noexcept {
    if (m_root_paddr == 0U || vaddr >= 0x40000000ULL) {
        return false;
    }

    const size_t l0_idx = (vaddr >> 39U) & 0x1FFU;
    const size_t l1_idx = (vaddr >> 30U) & 0x1FFU;
    const size_t l2_idx = (vaddr >> 21U) & 0x1FFU;
    const size_t l3_idx = (vaddr >> 12U) & 0x1FFU;

    auto* const l0_table = reinterpret_cast<uint64_t*>(phys_to_virt(m_root_paddr));
    if ((l0_table[l0_idx] & 1U) == 0U) {
        return false;
    }
    auto* const l1_table =
        reinterpret_cast<uint64_t*>(phys_to_virt(l0_table[l0_idx] & TABLE_ADDR_MASK));

    if ((l1_table[l1_idx] & 1U) == 0U) {
        return false;
    }
    auto* const l2_table =
        reinterpret_cast<uint64_t*>(phys_to_virt(l1_table[l1_idx] & TABLE_ADDR_MASK));

    if ((l2_table[l2_idx] & 1U) == 0U) {
        return false;
    }
    auto* const l3_table =
        reinterpret_cast<uint64_t*>(phys_to_virt(l2_table[l2_idx] & TABLE_ADDR_MASK));

    if ((l3_table[l3_idx] & 1U) == 0U) {
        return false;
    }

    l3_table[l3_idx] = 0ULL;
    flush_dcache_range(reinterpret_cast<uintptr_t>(&l3_table[l3_idx]), sizeof(uint64_t));

    asm volatile("tlbi vaae1is, %0; dsb ish; isb" ::"r"(vaddr >> 12U) : "memory");
    return true;
}

void AddressSpace::activate() const noexcept {
    if (m_root_paddr != 0U) {
        activate_root(m_root_paddr);
    }
}

void AddressSpace::activate_root(uintptr_t root_paddr) noexcept {
    asm volatile(
        "dsb ish\n\t"
        "msr ttbr0_el1, %0\n\t"
        "isb\n\t"
        "tlbi vmalle1is\n\t"
        "dsb ish\n\t"
        "isb" ::"r"(root_paddr)
        : "memory");
}

uintptr_t AddressSpace::current_root_paddr() noexcept {
    uintptr_t val = 0U;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(val));
    return val;
}

void AddressSpace::destroy() noexcept {
    if (m_root_paddr == 0U) {
        return;
    }

    auto* const l0 = reinterpret_cast<uint64_t*>(phys_to_virt(m_root_paddr));
    if ((l0[0] & 1U) != 0U) {
        const uintptr_t l1_paddr = l0[0] & TABLE_ADDR_MASK;
        auto* const l1 = reinterpret_cast<uint64_t*>(phys_to_virt(l1_paddr));

        // Only traverse L1[0] (user space). L1[1..511] are shared kernel mappings!
        if ((l1[0] & 1U) != 0U) {
            const uintptr_t l2_paddr = l1[0] & TABLE_ADDR_MASK;
            auto* const l2 = reinterpret_cast<uint64_t*>(phys_to_virt(l2_paddr));

            for (size_t i2 = 0U; i2 < PAGE_ENTRY_COUNT; ++i2) {
                // i2 == 45 is UART MMIO mirrored from kernel; do not free!
                if (i2 != 45U && (l2[i2] & 1U) != 0U) {
                    const uintptr_t l3_paddr = l2[i2] & TABLE_ADDR_MASK;
                    free_frame(l3_paddr);
                    l2[i2] = 0ULL;
                }
            }
            free_frame(l2_paddr);
            l1[0] = 0ULL;
        }
        free_frame(l1_paddr);
        l0[0] = 0ULL;
    }

    free_frame(m_root_paddr);
    m_root_paddr = 0U;
}

} // namespace Kernel::MM