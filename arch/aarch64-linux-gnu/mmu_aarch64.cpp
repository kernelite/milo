// File: arch/aarch64-linux-gnu/mmu_aarch64.cpp
#include "cpu_context.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "memory_config.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"

#include <cstddef>
#include <cstdint>

extern "C" {
extern char _text_start[];
extern char _text_end[];
extern char _rodata_start[];
extern char _rodata_end[];
extern char _data_start[];
extern char _data_end[];
// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern char __bss_start[];
// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern char __bss_end[];
}

namespace {

constexpr size_t PAGE_ENTRY_COUNT = 512U;
constexpr uint64_t TABLE_ADDR_MASK = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t BLOCK_2M_ADDR_MASK = 0x000FFFFFFFE00000ULL;
constexpr uint64_t DESC_TYPE_BLOCK = 0x1ULL;
constexpr uint64_t DESC_TYPE_TABLE = 0x3ULL;

constexpr uint64_t DESC_ATTR_NORMAL = 1ULL << 2U;
constexpr uint64_t AP_RO_BIT = 1ULL << 7U; // Access Permission[2] (1 = RO, 0 = RW)
constexpr uint64_t DESC_SH_ISH = 3ULL << 8U;
constexpr uint64_t DESC_AF = 1ULL << 10U;
constexpr uint64_t DESC_PXN = 1ULL << 53U;
constexpr uint64_t DESC_UXN = 1ULL << 54U;
constexpr uint64_t DESC_COW = 1ULL << 55U;
constexpr uint64_t PTE_DEMAND_ZERO = 1ULL << 56U;
constexpr uint64_t PTE_DZ_FLAGS_SHIFT = 8U;
constexpr uint64_t PTE_DZ_FLAGS_MASK = 0xFFULL << PTE_DZ_FLAGS_SHIFT;

constexpr uintptr_t USER_SPACE_MAX_VA = 0x0000000100000000ULL; // 4 GiB (L1 entries 0, 2, 3)
constexpr size_t USER_L1_SLOTS = 4U;
constexpr size_t KERNEL_RAM_L1_IDX = 1U; // 0x40000000..0x7FFFFFFF (identity kernel RAM)
constexpr size_t UART_L2_IDX = (0x09000000ULL >> 21U) & 0x1FFU;

constexpr bool is_allowed_user_vaddr(uintptr_t vaddr) noexcept {
    if (vaddr >= USER_SPACE_MAX_VA) {
        return false;
    }
    const size_t l0_idx = (vaddr >> 39U) & 0x1FFU;
    const size_t l1_idx = (vaddr >> 30U) & 0x1FFU;
    const size_t l2_idx = (vaddr >> 21U) & 0x1FFU;
    if (l0_idx != 0U || l1_idx == KERNEL_RAM_L1_IDX) {
        return false;
    }
    if (l1_idx == 0U && l2_idx == UART_L2_IDX) {
        return false;
    }
    return true;
}

constexpr uintptr_t align_down_page(uintptr_t addr) noexcept {
    return addr & ~(Arch::AArch64::PAGE_SIZE - 1U);
}

constexpr uintptr_t align_up_page(uintptr_t addr) noexcept {
    return (addr + Arch::AArch64::PAGE_SIZE - 1U) & ~(Arch::AArch64::PAGE_SIZE - 1U);
}

constexpr uint64_t encode_demand_zero_pte(HAL::PageFlags flags) noexcept {
    return PTE_DEMAND_ZERO |
           (static_cast<uint64_t>(static_cast<uint8_t>(flags)) << PTE_DZ_FLAGS_SHIFT);
}

inline bool is_mmu_enabled() noexcept {
    uint64_t sctlr = 0ULL;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (sctlr & 1ULL) != 0ULL;
}

inline uint64_t* table_ptr_from_phys(uintptr_t phys, bool mmu_on) noexcept {
    const uintptr_t addr = mmu_on ? Kernel::MM::phys_to_virt(phys) : phys;
    return reinterpret_cast<uint64_t*>(addr);
}

inline void flush_dcache_range(uintptr_t addr, size_t size) noexcept {
    const uintptr_t line_size = 64U;
    const uintptr_t end = addr + size;
    addr &= ~(line_size - 1U);
    for (; addr < end; addr += line_size) {
        asm volatile("dc civac, %0" ::"r"(addr) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

uintptr_t allocate_zeroed_table() noexcept {
    const uintptr_t phys = Kernel::MM::alloc_frame();
    if (phys == 0U) {
        return 0U;
    }

    auto* const table = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(phys));
    for (size_t i = 0U; i < PAGE_ENTRY_COUNT; ++i) {
        table[i] = 0ULL;
    }
    flush_dcache_range(reinterpret_cast<uintptr_t>(table), Arch::AArch64::PAGE_SIZE);
    return phys;
}

uintptr_t get_boot_ttbr0() noexcept {
    static uintptr_t boot_ttbr0 = 0U;
    if (boot_ttbr0 == 0U) {
        asm volatile("mrs %0, ttbr0_el1" : "=r"(boot_ttbr0));
    }
    return boot_ttbr0;
}

alignas(4096) uint64_t g_ttbr0_l0[512] = {0}; // TTBR0_EL1 (User / Lower VA space)
alignas(4096) uint64_t g_ttbr1_l0[512] = {0}; // TTBR1_EL1 (Kernel / Higher VA space)

uintptr_t allocate_table_page() noexcept {
    uintptr_t phys = Kernel::MM::alloc_frame();

    // Fallback static pool for early boot prior to frame allocator initialization
    if (phys == 0U) {
        alignas(4096) static uint64_t static_tables[64][512] = {};
        static size_t static_idx = 0U;
        if (static_idx < 64U) {
            phys = reinterpret_cast<uintptr_t>(&static_tables[static_idx++]);
        } else {
            return 0U;
        }
    }

    const bool mmu_on = is_mmu_enabled();
    auto* const table = table_ptr_from_phys(phys, mmu_on);
    for (size_t i = 0U; i < PAGE_ENTRY_COUNT; ++i) {
        table[i] = 0ULL;
    }
    flush_dcache_range(reinterpret_cast<uintptr_t>(table), Arch::AArch64::PAGE_SIZE);
    return phys;
}

uint64_t* ensure_l2_table(uint64_t* root_l0, size_t l0_idx, size_t l1_idx, bool mmu_on) noexcept {
    const uint64_t l0_desc = root_l0[l0_idx];
    if ((l0_desc & 1ULL) == 0ULL) {
        const uintptr_t l1_paddr = allocate_table_page();
        if (l1_paddr == 0U) {
            return nullptr;
        }
        root_l0[l0_idx] = (l1_paddr & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&root_l0[l0_idx]), sizeof(uint64_t));
    } else if ((l0_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return nullptr;
    }

    auto* const l1_table = table_ptr_from_phys(root_l0[l0_idx] & TABLE_ADDR_MASK, mmu_on);
    const uint64_t l1_desc = l1_table[l1_idx];
    if ((l1_desc & 1ULL) == 0ULL) {
        const uintptr_t l2_paddr = allocate_table_page();
        if (l2_paddr == 0U) {
            return nullptr;
        }
        l1_table[l1_idx] = (l2_paddr & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&l1_table[l1_idx]), sizeof(uint64_t));
    } else if ((l1_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return nullptr;
    }

    return table_ptr_from_phys(l1_table[l1_idx] & TABLE_ADDR_MASK, mmu_on);
}

bool map_block_2m_in_root(uint64_t* root_l0, uintptr_t virt_addr, uintptr_t phys_addr) noexcept {
    if (((virt_addr | phys_addr) & (Arch::AArch64::BLOCK_SIZE_2M - 1U)) != 0U) {
        return false;
    }

    const size_t l0_idx = (virt_addr >> 39U) & 0x1FFU;
    const size_t l1_idx = (virt_addr >> 30U) & 0x1FFU;
    const size_t l2_idx = (virt_addr >> 21U) & 0x1FFU;
    const bool mmu_on = is_mmu_enabled();

    uint64_t* const l2_table = ensure_l2_table(root_l0, l0_idx, l1_idx, mmu_on);
    if (l2_table == nullptr) {
        return false;
    }

    // Refuse to overwrite an existing L3 table descriptor with a 2 MiB block
    if ((l2_table[l2_idx] & 1ULL) != 0ULL &&
        (l2_table[l2_idx] & DESC_TYPE_TABLE) == DESC_TYPE_TABLE) {
        return false;
    }

    const uint64_t desc = (phys_addr & BLOCK_2M_ADDR_MASK) | DESC_TYPE_BLOCK | DESC_ATTR_NORMAL |
                          DESC_SH_ISH | DESC_AF | DESC_PXN | DESC_UXN;
    l2_table[l2_idx] = desc;
    flush_dcache_range(reinterpret_cast<uintptr_t>(&l2_table[l2_idx]), sizeof(uint64_t));
    return true;
}

bool map_page_in_root(uint64_t* root_l0, uintptr_t virt_addr, uintptr_t phys_addr,
                      HAL::PageFlags flags) noexcept {
    const size_t l0_idx = (virt_addr >> 39U) & 0x1FFU;
    const size_t l1_idx = (virt_addr >> 30U) & 0x1FFU;
    const size_t l2_idx = (virt_addr >> 21U) & 0x1FFU;
    const size_t l3_idx = (virt_addr >> 12U) & 0x1FFU;
    const bool mmu_on = is_mmu_enabled();

    uint64_t* const l2_table = ensure_l2_table(root_l0, l0_idx, l1_idx, mmu_on);
    if (l2_table == nullptr) {
        return false;
    }

    const uint64_t l2_desc = l2_table[l2_idx];
    if ((l2_desc & 1ULL) == 0ULL) {
        const uintptr_t l3_paddr = allocate_table_page();
        if (l3_paddr == 0U) {
            return false;
        }
        l2_table[l2_idx] = (l3_paddr & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&l2_table[l2_idx]), sizeof(uint64_t));
    } else if ((l2_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        // L2 entry is a 2 MiB block descriptor (0b01), not an L3 table descriptor (0b11)
        return false;
    }

    auto* const l3_table = table_ptr_from_phys(l2_table[l2_idx] & TABLE_ADDR_MASK, mmu_on);

    uint64_t desc = (phys_addr & TABLE_ADDR_MASK) | DESC_TYPE_TABLE; // Valid + Page Descriptor
    desc |= DESC_AF;

    const bool is_write = HAL::has_flag(flags, HAL::PageFlags::Write);
    const bool is_user = HAL::has_flag(flags, HAL::PageFlags::User);
    const bool is_exec = HAL::has_flag(flags, HAL::PageFlags::Execute);
    const bool is_device = HAL::has_flag(flags, HAL::PageFlags::Device);

    const uint64_t attr_idx = is_device ? 0ULL : 1ULL;
    desc |= (attr_idx << 2U);

    uint64_t ap_bits = 0ULL;
    if (!is_write) {
        ap_bits = is_user ? 0x3ULL : 0x2ULL;
    } else {
        ap_bits = is_user ? 0x1ULL : 0x0ULL;
    }
    desc |= (ap_bits << 6U);

    if (!is_device) {
        desc |= DESC_SH_ISH;
    }

    if (!is_exec) {
        desc |= DESC_PXN | DESC_UXN;
    } else if (is_user) {
        desc |= DESC_PXN;
    } else {
        desc |= DESC_UXN;
    }

    l3_table[l3_idx] = desc;
    flush_dcache_range(reinterpret_cast<uintptr_t>(&l3_table[l3_idx]), sizeof(uint64_t));
    return true;
}

bool unmap_page_in_root(const uint64_t* root_l0, uintptr_t virt_addr) noexcept {
    const size_t l0_idx = (virt_addr >> 39U) & 0x1FFU;
    const size_t l1_idx = (virt_addr >> 30U) & 0x1FFU;
    const size_t l2_idx = (virt_addr >> 21U) & 0x1FFU;
    const size_t l3_idx = (virt_addr >> 12U) & 0x1FFU;
    const bool mmu_on = is_mmu_enabled();

    const uint64_t l0_desc = root_l0[l0_idx];
    if ((l0_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }
    const auto* const l1_table = table_ptr_from_phys(l0_desc & TABLE_ADDR_MASK, mmu_on);

    const uint64_t l1_desc = l1_table[l1_idx];
    if ((l1_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }
    const auto* const l2_table = table_ptr_from_phys(l1_desc & TABLE_ADDR_MASK, mmu_on);

    const uint64_t l2_desc = l2_table[l2_idx];
    if ((l2_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }
    auto* const l3_table = table_ptr_from_phys(l2_desc & TABLE_ADDR_MASK, mmu_on);

    l3_table[l3_idx] = 0ULL;
    flush_dcache_range(reinterpret_cast<uintptr_t>(&l3_table[l3_idx]), sizeof(uint64_t));

    asm volatile("tlbi vaae1is, %0; dsb ish; isb" ::"r"(virt_addr >> 12U) : "memory");
    return true;
}

void map_ram_range_hybrid(uint64_t* root_l0, uintptr_t va_offset, uintptr_t range_start,
                          uintptr_t range_end, uintptr_t excl_start, uintptr_t excl_end) noexcept {
    const HAL::PageFlags ram_rw = HAL::PageFlags::Read | HAL::PageFlags::Write;
    uintptr_t addr = range_start;

    while (addr < range_end) {
        if (addr >= excl_start && addr < excl_end) {
            addr = excl_end;
            continue;
        }

        const bool is_2m_aligned = (addr & (Arch::AArch64::BLOCK_SIZE_2M - 1U)) == 0U;
        const bool fits_in_range = (range_end - addr) >= Arch::AArch64::BLOCK_SIZE_2M;
        const uintptr_t block_end = addr + Arch::AArch64::BLOCK_SIZE_2M;
        const bool overlaps_excl = (addr < excl_end) && (block_end > excl_start);

        if (is_2m_aligned && fits_in_range && !overlaps_excl) {
            map_block_2m_in_root(root_l0, addr + va_offset, addr);
            addr = block_end;
        } else {
            map_page_in_root(root_l0, addr + va_offset, addr, ram_rw);
            addr += Arch::AArch64::PAGE_SIZE;
        }
    }
}

class AArch64MemoryControl : public HAL::MemoryControl {
  public:
    constexpr AArch64MemoryControl() = default;

    bool map(uintptr_t virt_addr, uintptr_t phys_addr, HAL::PageFlags flags) override {
        if ((virt_addr & (1ULL << 63U)) != 0U) {
            return map_page_in_root(g_ttbr1_l0, virt_addr, phys_addr, flags);
        }
        return map_page_in_root(g_ttbr0_l0, virt_addr, phys_addr, flags);
    }

    bool unmap(uintptr_t virt_addr) override {
        if ((virt_addr & (1ULL << 63U)) != 0U) {
            return unmap_page_in_root(g_ttbr1_l0, virt_addr);
        }
        return unmap_page_in_root(g_ttbr0_l0, virt_addr);
    }

    void activate() override {
        const HAL::PlatformInfo platform = HAL::get_platform_info();
        const uintptr_t text_start = reinterpret_cast<uintptr_t>(_text_start);
        const uintptr_t text_end = reinterpret_cast<uintptr_t>(_text_end);
        const uintptr_t rodata_start = reinterpret_cast<uintptr_t>(_rodata_start);
        const uintptr_t rodata_end = reinterpret_cast<uintptr_t>(_rodata_end);
        const uintptr_t data_start = reinterpret_cast<uintptr_t>(_data_start);
        const uintptr_t bss_end = reinterpret_cast<uintptr_t>(__bss_end);

        const uintptr_t kernel_start = align_down_page(text_start);
        const uintptr_t kernel_end = align_up_page(bss_end);

        const uintptr_t ram_base = platform.ram_base;
        const uintptr_t ram_end = platform.ram_base + platform.ram_size;
        const uintptr_t higher_half_offset = platform.higher_half_offset;

        const HAL::PageFlags dev_rw =
            HAL::PageFlags::Read | HAL::PageFlags::Write | HAL::PageFlags::Device;

        // 1. Map GICv2 Distributor (0x08000000), CPU Interface (0x08010000), and UART0 (0x09000000)
        map_page_in_root(g_ttbr0_l0, 0x08000000U, 0x08000000U, dev_rw);
        map_page_in_root(g_ttbr0_l0, 0x08010000U, 0x08010000U, dev_rw);
        map_page_in_root(g_ttbr0_l0, 0x09000000U, 0x09000000U, dev_rw);

        map_page_in_root(g_ttbr1_l0, 0x08000000ULL + higher_half_offset, 0x08000000U, dev_rw);
        map_page_in_root(g_ttbr1_l0, 0x08010000ULL + higher_half_offset, 0x08010000U, dev_rw);
        map_page_in_root(g_ttbr1_l0, 0x09000000ULL + higher_half_offset, 0x09000000U, dev_rw);

        // 2. Map Kernel ELF Segments at 4 KiB L3 granularity (enforcing W^X) in TTBR0 and TTBR1
        for (uintptr_t addr = text_start; addr < text_end; addr += Arch::AArch64::PAGE_SIZE) {
            map_page_in_root(g_ttbr0_l0, addr, addr,
                             HAL::PageFlags::Read | HAL::PageFlags::Execute);
            map_page_in_root(g_ttbr1_l0, addr + higher_half_offset, addr,
                             HAL::PageFlags::Read | HAL::PageFlags::Execute);
        }

        for (uintptr_t addr = rodata_start; addr < rodata_end; addr += Arch::AArch64::PAGE_SIZE) {
            map_page_in_root(g_ttbr0_l0, addr, addr, HAL::PageFlags::Read);
            map_page_in_root(g_ttbr1_l0, addr + higher_half_offset, addr, HAL::PageFlags::Read);
        }

        for (uintptr_t addr = data_start; addr < kernel_end; addr += Arch::AArch64::PAGE_SIZE) {
            map_page_in_root(g_ttbr0_l0, addr, addr, HAL::PageFlags::Read | HAL::PageFlags::Write);
            map_page_in_root(g_ttbr1_l0, addr + higher_half_offset, addr,
                             HAL::PageFlags::Read | HAL::PageFlags::Write);
        }

        // 3. Map full discovered physical RAM outside the kernel image using 2 MiB L2 blocks
        //    for aligned spans and 4 KiB L3 pages for unaligned leading/trailing boundaries.
        map_ram_range_hybrid(g_ttbr0_l0, 0U, ram_base, ram_end, kernel_start, kernel_end);
        map_ram_range_hybrid(g_ttbr1_l0, higher_half_offset, ram_base, ram_end, kernel_start,
                             kernel_end);

        Arch::AArch64::init_mmu(reinterpret_cast<uintptr_t>(g_ttbr0_l0),
                                reinterpret_cast<uintptr_t>(g_ttbr1_l0));
    }

    HAL::MmuStatus status() const noexcept override {
        uint64_t sctlr = 0ULL;
        asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

        return HAL::MmuStatus{.mmu_enabled = (sctlr & (1ULL << 0U)) != 0U,
                              .dcache_enabled = (sctlr & (1ULL << 2U)) != 0U,
                              .icache_enabled = (sctlr & (1ULL << 12U)) != 0U,
                              .raw_control_reg = sctlr};
    }
};

constinit AArch64MemoryControl aarch64_mmu{};

} // namespace

namespace Arch::AArch64 {

void init_mmu(uintptr_t ttbr0_base, uintptr_t ttbr1_base) {
    flush_dcache_range(ttbr0_base, 4096U);
    flush_dcache_range(ttbr1_base, 4096U);

    const uint64_t mair = (0x00ULL << 0U) | (0xFFULL << 8U);
    asm volatile("msr mair_el1, %0" ::"r"(mair));

    uint64_t mmfr0 = 0ULL;
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    const uint64_t pa_range = mmfr0 & 0xFU;

    const uint64_t tcr = (16ULL << 0U) | (1ULL << 8U) | (1ULL << 10U) | (3ULL << 12U) |
                         (0ULL << 14U) | (16ULL << 16U) | (1ULL << 24U) | (1ULL << 26U) |
                         (3ULL << 28U) | (2ULL << 30U) | (pa_range << 32U);
    asm volatile("msr tcr_el1, %0" ::"r"(tcr));

    asm volatile("msr ttbr0_el1, %0" ::"r"(ttbr0_base));
    asm volatile("msr ttbr1_el1, %0" ::"r"(ttbr1_base));

    asm volatile("dsb sy\n\t"
                 "tlbi vmalle1is\n\t"
                 "dsb sy\n\t"
                 "isb" ::
                     : "memory");

    uint64_t sctlr = 0ULL;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0U);
    sctlr |= (1ULL << 2U);
    sctlr |= (1ULL << 12U);
    sctlr &= ~(1ULL << 1U);
    sctlr &= ~(1ULL << 3U);
    sctlr &= ~(1ULL << 4U);
    asm volatile("msr sctlr_el1, %0\n\t"
                 "isb" ::"r"(sctlr)
                 : "memory");
}

} // namespace Arch::AArch64

namespace HAL {
MemoryControl& get_mmu() noexcept {
    return aarch64_mmu;
}

bool AddressSpaceBackend::map_demand_zero(uintptr_t root_paddr, uintptr_t vaddr,
                                          HAL::PageFlags expected_flags) noexcept {
    if (root_paddr == 0U || !is_allowed_user_vaddr(vaddr)) {
        return false;
    }

    const size_t l0_idx = (vaddr >> 39U) & 0x1FFU;
    const size_t l1_idx = (vaddr >> 30U) & 0x1FFU;
    const size_t l2_idx = (vaddr >> 21U) & 0x1FFU;
    const size_t l3_idx = (vaddr >> 12U) & 0x1FFU;

    const bool mmu_on = is_mmu_enabled();
    auto* const root_l0 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(root_paddr));

    uint64_t* const l2_table = ensure_l2_table(root_l0, l0_idx, l1_idx, mmu_on);
    if (l2_table == nullptr) {
        return false;
    }

    const uint64_t l2_desc = l2_table[l2_idx];
    if ((l2_desc & 1ULL) == 0ULL) {
        const uintptr_t l3_paddr = allocate_table_page();
        if (l3_paddr == 0U) {
            return false;
        }
        l2_table[l2_idx] = (l3_paddr & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&l2_table[l2_idx]), sizeof(uint64_t));
    } else if ((l2_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    auto* const l3_table = table_ptr_from_phys(l2_table[l2_idx] & TABLE_ADDR_MASK, mmu_on);
    l3_table[l3_idx] = encode_demand_zero_pte(expected_flags);
    flush_dcache_range(reinterpret_cast<uintptr_t>(&l3_table[l3_idx]), sizeof(uint64_t));
    return true;
}

bool AddressSpaceBackend::resolve_cow_page(uintptr_t root_paddr, uintptr_t vaddr,
                                           uintptr_t& out_paddr) noexcept {
    if (root_paddr == 0U || !is_allowed_user_vaddr(vaddr)) {
        return false;
    }

    uint64_t raw_pte = 0ULL;
    if (!query_pte_flags(root_paddr, vaddr, raw_pte) || (raw_pte & 1ULL) == 0ULL ||
        (raw_pte & DESC_COW) == 0ULL) {
        return false;
    }

    const uintptr_t fault_paddr = raw_pte & TABLE_ADDR_MASK;
    Kernel::MM::Page* const page = Kernel::MM::page_from_paddr(fault_paddr);
    if (page == nullptr) {
        return false;
    }

    HAL::PageFlags user_rw_flags =
        HAL::PageFlags::User | HAL::PageFlags::Read | HAL::PageFlags::Write;
    if ((raw_pte & DESC_UXN) == 0ULL) {
        user_rw_flags |= HAL::PageFlags::Execute;
    }
    if (((raw_pte >> 2U) & 0x7ULL) == 0ULL) {
        user_rw_flags |= HAL::PageFlags::Device;
    } else {
        user_rw_flags |= HAL::PageFlags::Cacheable;
    }

    const uintptr_t page_offset = vaddr & (Arch::AArch64::PAGE_SIZE - 1U);
    if (__atomic_load_n(&page->ref_count, __ATOMIC_ACQUIRE) > 1U) {
        const uintptr_t new_paddr = Kernel::MM::alloc_frame();
        if (new_paddr == 0U) {
            return false;
        }

        auto* const dst_kva = reinterpret_cast<void*>(Kernel::MM::phys_to_virt(new_paddr));
        const auto* const src_kva =
            reinterpret_cast<const void*>(Kernel::MM::phys_to_virt(fault_paddr));
        __builtin_memcpy(dst_kva, src_kva, Arch::AArch64::PAGE_SIZE);
        flush_dcache_range(reinterpret_cast<uintptr_t>(dst_kva), Arch::AArch64::PAGE_SIZE);

        if (!remap_user_page(root_paddr, vaddr, new_paddr, user_rw_flags)) {
            Kernel::MM::free_frame(new_paddr);
            return false;
        }
        Kernel::MM::page_put(fault_paddr);
        out_paddr = new_paddr | page_offset;
    } else {
        if (!remap_user_page(root_paddr, vaddr, fault_paddr, user_rw_flags)) {
            return false;
        }
        out_paddr = fault_paddr | page_offset;
    }

    asm volatile("dsb ish\n\t"
                 "tlbi vaae1is, %0\n\t"
                 "dsb ish\n\t"
                 "isb" ::"r"(vaddr >> 12U)
                 : "memory");
    return true;
}

bool AddressSpaceBackend::resolve_demand_zero_page(uintptr_t root_paddr, uintptr_t vaddr,
                                                   bool require_write,
                                                   uintptr_t& out_paddr) noexcept {
    if (root_paddr == 0U || !is_allowed_user_vaddr(vaddr)) {
        return false;
    }

    uint64_t raw_pte = 0ULL;
    if (!query_pte_flags(root_paddr, vaddr, raw_pte) || (raw_pte & 1ULL) != 0ULL ||
        (raw_pte & PTE_DEMAND_ZERO) == 0ULL) {
        return false;
    }

    const auto intended_flags =
        static_cast<HAL::PageFlags>((raw_pte & PTE_DZ_FLAGS_MASK) >> PTE_DZ_FLAGS_SHIFT);
    if (!HAL::has_flag(intended_flags, HAL::PageFlags::User)) {
        return false;
    }
    if (require_write && !HAL::has_flag(intended_flags, HAL::PageFlags::Write)) {
        return false;
    }
    if (!require_write && !HAL::has_flag(intended_flags, HAL::PageFlags::Read)) {
        return false;
    }

    const uintptr_t new_paddr = Kernel::MM::alloc_frame();
    if (new_paddr == 0U) {
        return false;
    }

    auto* const dst_kva = reinterpret_cast<void*>(Kernel::MM::phys_to_virt(new_paddr));
    __builtin_memset(dst_kva, 0, Arch::AArch64::PAGE_SIZE);
    flush_dcache_range(reinterpret_cast<uintptr_t>(dst_kva), Arch::AArch64::PAGE_SIZE);

    if (!map_user_page(root_paddr, vaddr, new_paddr, intended_flags)) {
        Kernel::MM::free_frame(new_paddr);
        return false;
    }

    out_paddr = new_paddr | (vaddr & (Arch::AArch64::PAGE_SIZE - 1U));
    return true;
}

uintptr_t AddressSpaceBackend::create_root() noexcept {
    const uintptr_t root_phys = allocate_zeroed_table();
    if (root_phys == 0U) {
        return 0U;
    }

    const uintptr_t boot_ttbr0 = get_boot_ttbr0();
    if (boot_ttbr0 != 0U) {
        auto* const new_l0 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(root_phys));
        const auto* const k_l0 =
            reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(boot_ttbr0));

        const uintptr_t l1_phys = allocate_zeroed_table();
        if (l1_phys == 0U) {
            Kernel::MM::free_frame(root_phys);
            return 0U;
        }

        new_l0[0] = (l1_phys & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&new_l0[0]), sizeof(uint64_t));

        auto* const new_l1 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(l1_phys));
        const auto* const k_l1 =
            reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(k_l0[0] & TABLE_ADDR_MASK));

        for (size_t i = 1U; i < PAGE_ENTRY_COUNT; ++i) {
            new_l1[i] = k_l1[i];
        }

        const uintptr_t l2_phys = allocate_zeroed_table();
        if (l2_phys != 0U) {
            new_l1[0] = (l2_phys & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
            auto* const new_l2 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(l2_phys));
            const auto* const k_l2 = reinterpret_cast<const uint64_t*>(
                Kernel::MM::phys_to_virt(k_l1[0] & TABLE_ADDR_MASK));

            new_l2[UART_L2_IDX] = k_l2[UART_L2_IDX];
            flush_dcache_range(reinterpret_cast<uintptr_t>(new_l2), Arch::AArch64::PAGE_SIZE);
        }
        flush_dcache_range(reinterpret_cast<uintptr_t>(new_l1), Arch::AArch64::PAGE_SIZE);
    }

    return root_phys;
}

bool AddressSpaceBackend::map_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t paddr,
                                        HAL::PageFlags flags) noexcept {
    if (root_paddr == 0U || !is_allowed_user_vaddr(vaddr)) {
        return false;
    }
    auto* const root_l0 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(root_paddr));
    return map_page_in_root(root_l0, vaddr, paddr, flags);
}

bool AddressSpaceBackend::unmap_user_page(uintptr_t root_paddr, uintptr_t vaddr) noexcept {
    if (root_paddr == 0U || !is_allowed_user_vaddr(vaddr)) {
        return false;
    }
    const auto* const root_l0 =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(root_paddr));
    return unmap_page_in_root(root_l0, vaddr);
}

bool AddressSpaceBackend::query_user_page(uintptr_t root_paddr, uintptr_t vaddr,
                                          uintptr_t& out_paddr,
                                          HAL::PageFlags& out_flags) noexcept {
    constexpr uintptr_t PAGE_OFFSET_MASK = Arch::AArch64::PAGE_SIZE - 1U;
    constexpr uint64_t AP_USER_BIT = 1ULL << 6U;
    constexpr uint64_t UXN_BIT = 1ULL << 54U;
    constexpr uint64_t ATTR_INDEX_MASK = 0x7ULL;

    if (root_paddr == 0U || vaddr >= USER_SPACE_MAX_VA) {
        return false;
    }

    const size_t l0_idx = (vaddr >> 39U) & 0x1FFU;
    const size_t l1_idx = (vaddr >> 30U) & 0x1FFU;
    const size_t l2_idx = (vaddr >> 21U) & 0x1FFU;
    const size_t l3_idx = (vaddr >> 12U) & 0x1FFU;

    const auto* const l0_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(root_paddr));
    const uint64_t l0_desc = l0_table[l0_idx];
    if ((l0_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    const auto* const l1_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(l0_desc & TABLE_ADDR_MASK));
    const uint64_t l1_desc = l1_table[l1_idx];
    if ((l1_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    const auto* const l2_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(l1_desc & TABLE_ADDR_MASK));
    const uint64_t l2_desc = l2_table[l2_idx];
    if ((l2_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    const auto* const l3_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(l2_desc & TABLE_ADDR_MASK));
    const uint64_t l3_desc = l3_table[l3_idx];
    if ((l3_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    if ((l3_desc & AP_USER_BIT) == 0ULL) {
        return false;
    }

    out_paddr = (l3_desc & TABLE_ADDR_MASK) | (vaddr & PAGE_OFFSET_MASK);

    HAL::PageFlags flags = HAL::PageFlags::None;
    if ((l3_desc & AP_USER_BIT) != 0ULL) {
        flags |= HAL::PageFlags::User | HAL::PageFlags::Read;
        if ((l3_desc & AP_RO_BIT) == 0ULL) {
            flags |= HAL::PageFlags::Write;
        }
    }
    if ((l3_desc & UXN_BIT) == 0ULL) {
        flags |= HAL::PageFlags::Execute;
    }

    const uint64_t attr_idx = (l3_desc >> 2U) & ATTR_INDEX_MASK;
    if (attr_idx == 0ULL) {
        flags |= HAL::PageFlags::Device;
    } else {
        flags |= HAL::PageFlags::Cacheable;
    }

    out_flags = flags;
    return true;
}

bool AddressSpaceBackend::query_pte_flags(uintptr_t root_paddr, uintptr_t vaddr,
                                          uint64_t& out_raw_pte) noexcept {
    constexpr uint64_t AP_USER_BIT = 1ULL << 6U;

    if (root_paddr == 0U || vaddr >= USER_SPACE_MAX_VA) {
        return false;
    }

    const size_t l0_idx = (vaddr >> 39U) & 0x1FFU;
    const size_t l1_idx = (vaddr >> 30U) & 0x1FFU;
    const size_t l2_idx = (vaddr >> 21U) & 0x1FFU;
    const size_t l3_idx = (vaddr >> 12U) & 0x1FFU;

    const auto* const l0_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(root_paddr));
    const uint64_t l0_desc = l0_table[l0_idx];
    if ((l0_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    const auto* const l1_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(l0_desc & TABLE_ADDR_MASK));
    const uint64_t l1_desc = l1_table[l1_idx];
    if ((l1_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    const auto* const l2_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(l1_desc & TABLE_ADDR_MASK));
    const uint64_t l2_desc = l2_table[l2_idx];
    if ((l2_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    const auto* const l3_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(l2_desc & TABLE_ADDR_MASK));
    const uint64_t l3_desc = l3_table[l3_idx];

    if ((l3_desc & 1ULL) == 0ULL) {
        if ((l3_desc & PTE_DEMAND_ZERO) != 0ULL) {
            out_raw_pte = l3_desc;
            return true;
        }
        return false;
    }

    if ((l3_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE || (l3_desc & AP_USER_BIT) == 0ULL) {
        return false;
    }

    out_raw_pte = l3_desc;
    return true;
}

bool AddressSpaceBackend::remap_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t paddr,
                                          HAL::PageFlags flags) noexcept {
    if (root_paddr == 0U || !is_allowed_user_vaddr(vaddr)) {
        return false;
    }

    auto* const root_l0 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(root_paddr));
    return map_page_in_root(root_l0, vaddr, paddr, flags);
}

void AddressSpaceBackend::activate_root(uintptr_t root_paddr) noexcept {
    asm volatile("dsb ish\n\t"
                 "msr ttbr0_el1, %0\n\t"
                 "isb\n\t"
                 "tlbi vmalle1is\n\t"
                 "dsb ish\n\t"
                 "isb" ::"r"(root_paddr)
                 : "memory");
}

uintptr_t AddressSpaceBackend::current_root_paddr() noexcept {
    uintptr_t val = 0U;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(val));
    return val;
}

void AddressSpaceBackend::destroy_root(uintptr_t root_paddr) noexcept {
    if (root_paddr == 0U) {
        return;
    }

    auto* const lvl0 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(root_paddr));
    if ((lvl0[0] & 1U) != 0U) {
        const uintptr_t l1_paddr = lvl0[0] & TABLE_ADDR_MASK;
        auto* const lvl1 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(l1_paddr));

        for (size_t i1 = 0U; i1 < USER_L1_SLOTS; ++i1) {
            if (i1 == KERNEL_RAM_L1_IDX || (lvl1[i1] & 1U) == 0U) {
                continue;
            }
            const uintptr_t l2_paddr = lvl1[i1] & TABLE_ADDR_MASK;
            auto* const lvl2 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(l2_paddr));

            for (size_t i2 = 0U; i2 < PAGE_ENTRY_COUNT; ++i2) {
                if (i1 == 0U && i2 == UART_L2_IDX) {
                    continue;
                }
                if ((lvl2[i2] & 1U) != 0U) {
                    const uintptr_t l3_paddr = lvl2[i2] & TABLE_ADDR_MASK;
                    auto* const lvl3 =
                        reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(l3_paddr));
                    for (size_t i3 = 0U; i3 < PAGE_ENTRY_COUNT; ++i3) {
                        if ((lvl3[i3] & 1U) != 0U) {
                            const uintptr_t page_paddr = lvl3[i3] & TABLE_ADDR_MASK;
                            if (Kernel::MM::pfa_is_page_allocated(page_paddr)) {
                                Kernel::MM::free_frame(page_paddr);
                            }
                            lvl3[i3] = 0ULL;
                        }
                    }
                    Kernel::MM::free_frame(l3_paddr);
                    lvl2[i2] = 0ULL;
                }
            }
            Kernel::MM::free_frame(l2_paddr);
            lvl1[i1] = 0ULL;
        }
        Kernel::MM::free_frame(l1_paddr);
        lvl0[0] = 0ULL;
    }

    Kernel::MM::free_frame(root_paddr);
}

bool AddressSpaceBackend::clone_user_space(uintptr_t src_root, uintptr_t dst_root) noexcept {
    if (src_root == 0U || dst_root == 0U) {
        return false;
    }

    const auto* const src_l0 =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(src_root));
    if ((src_l0[0] & 1U) == 0U) {
        return true;
    }

    const auto* const src_l1 =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(src_l0[0] & TABLE_ADDR_MASK));

    auto* const dst_l0 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(dst_root));
    if ((dst_l0[0] & 1U) == 0U) {
        return false;
    }
    auto* const dst_l1 =
        reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(dst_l0[0] & TABLE_ADDR_MASK));

    for (size_t i1 = 0U; i1 < USER_L1_SLOTS; ++i1) {
        if (i1 == KERNEL_RAM_L1_IDX || (src_l1[i1] & 1U) == 0U) {
            continue;
        }

        const auto* const src_l2 = reinterpret_cast<const uint64_t*>(
            Kernel::MM::phys_to_virt(src_l1[i1] & TABLE_ADDR_MASK));

        uintptr_t dst_l2_paddr = 0U;
        if ((dst_l1[i1] & 1U) == 0U) {
            dst_l2_paddr = allocate_zeroed_table();
            if (dst_l2_paddr == 0U) {
                return false;
            }
            dst_l1[i1] = (dst_l2_paddr & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
            flush_dcache_range(reinterpret_cast<uintptr_t>(&dst_l1[i1]), sizeof(uint64_t));
        } else {
            dst_l2_paddr = dst_l1[i1] & TABLE_ADDR_MASK;
        }

        auto* const dst_l2 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(dst_l2_paddr));

        for (size_t i2 = 0U; i2 < PAGE_ENTRY_COUNT; ++i2) {
            if ((i1 == 0U && i2 == UART_L2_IDX) || (src_l2[i2] & 1U) == 0U) {
                continue;
            }

            auto* const src_l3 =
                reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(src_l2[i2] & TABLE_ADDR_MASK));

            uintptr_t dst_l3_paddr = 0U;
            if ((dst_l2[i2] & 1U) == 0U) {
                dst_l3_paddr = allocate_zeroed_table();
                if (dst_l3_paddr == 0U) {
                    return false;
                }
                dst_l2[i2] = (dst_l3_paddr & TABLE_ADDR_MASK) | DESC_TYPE_TABLE;
                flush_dcache_range(reinterpret_cast<uintptr_t>(&dst_l2[i2]), sizeof(uint64_t));
            } else {
                dst_l3_paddr = dst_l2[i2] & TABLE_ADDR_MASK;
            }

            auto* const dst_l3 =
                reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(dst_l3_paddr));

            for (size_t i3 = 0U; i3 < PAGE_ENTRY_COUNT; ++i3) {
                uint64_t src_desc = src_l3[i3];
                if ((src_desc & 1U) == 0U) {
                    if ((src_desc & PTE_DEMAND_ZERO) != 0ULL) {
                        dst_l3[i3] = src_desc;
                    }
                    continue;
                }

                const uintptr_t src_page_paddr = src_desc & TABLE_ADDR_MASK;

                // If the page is writable (AP[2] == 0), downgrade it to Read-Only and mark as CoW
                if ((src_desc & AP_RO_BIT) == 0ULL) {
                    src_desc |= AP_RO_BIT; // Make Read-Only
                    src_desc |= DESC_COW;  // Mark as CoW

                    // Update parent PTE
                    src_l3[i3] = src_desc;
                    flush_dcache_range(reinterpret_cast<uintptr_t>(&src_l3[i3]), sizeof(uint64_t));
                }

                // Map the exact same physical frame into the child's L3 table
                dst_l3[i3] = src_desc;

                // Increment the physical frame's reference count
                Kernel::MM::page_get(src_page_paddr);
            }

            flush_dcache_range(reinterpret_cast<uintptr_t>(dst_l3), Arch::AArch64::PAGE_SIZE);
        }
    }

    asm volatile("dsb ish\n\t"
                 "tlbi vmalle1is\n\t"
                 "dsb ish\n\t"
                 "isb" ::
                     : "memory");
    return true;
}

bool AddressSpaceBackend::protect_user_page(uintptr_t root_paddr, uintptr_t vaddr,
                                            HAL::PageFlags flags) noexcept {
    if (root_paddr == 0U || !is_allowed_user_vaddr(vaddr)) {
        return false;
    }

    const size_t l0_idx = (vaddr >> 39U) & 0x1FFU;
    const size_t l1_idx = (vaddr >> 30U) & 0x1FFU;
    const size_t l2_idx = (vaddr >> 21U) & 0x1FFU;
    const size_t l3_idx = (vaddr >> 12U) & 0x1FFU;

    const auto* const l0_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(root_paddr));
    const uint64_t l0_desc = l0_table[l0_idx];
    if ((l0_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    const auto* const l1_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(l0_desc & TABLE_ADDR_MASK));
    const uint64_t l1_desc = l1_table[l1_idx];
    if ((l1_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    const auto* const l2_table =
        reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(l1_desc & TABLE_ADDR_MASK));
    const uint64_t l2_desc = l2_table[l2_idx];
    if ((l2_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    auto* const l3_table =
        reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(l2_desc & TABLE_ADDR_MASK));
    uint64_t l3_desc = l3_table[l3_idx];

    if ((l3_desc & 1ULL) == 0ULL) {
        if ((l3_desc & PTE_DEMAND_ZERO) != 0ULL) {
            l3_table[l3_idx] = encode_demand_zero_pte(flags);
            flush_dcache_range(reinterpret_cast<uintptr_t>(&l3_table[l3_idx]), sizeof(uint64_t));
            return true;
        }
        return false;
    }

    if ((l3_desc & DESC_TYPE_TABLE) != DESC_TYPE_TABLE) {
        return false;
    }

    const bool is_write = HAL::has_flag(flags, HAL::PageFlags::Write);
    const bool is_user = HAL::has_flag(flags, HAL::PageFlags::User);
    const bool is_exec = HAL::has_flag(flags, HAL::PageFlags::Execute);

    uint64_t ap_bits = 0ULL;
    if (!is_write) {
        ap_bits = is_user ? 0x3ULL : 0x2ULL;
    } else {
        ap_bits = is_user ? 0x1ULL : 0x0ULL;
    }
    l3_desc = (l3_desc & ~(0x3ULL << 6U)) | (ap_bits << 6U);

    if (!is_exec) {
        l3_desc |= (1ULL << 53U) | (1ULL << 54U);
    } else if (is_user) {
        l3_desc = (l3_desc & ~(1ULL << 54U)) | (1ULL << 53U);
    } else {
        l3_desc = (l3_desc & ~(1ULL << 53U)) | (1ULL << 54U);
    }

    l3_table[l3_idx] = l3_desc;
    flush_dcache_range(reinterpret_cast<uintptr_t>(&l3_table[l3_idx]), sizeof(uint64_t));

    asm volatile("dsb ish\n\t"
                 "tlbi vaae1is, %0\n\t"
                 "dsb ish\n\t"
                 "isb" ::"r"(vaddr >> 12U)
                 : "memory");
    return true;
}

} // namespace HAL