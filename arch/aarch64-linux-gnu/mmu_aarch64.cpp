#include "cpu_context.hpp"
#include "hal/console.hpp"
#include "hal/mmu.hpp"
#include "mm/pfa.hpp"
#include "mm/address_space.hpp"
#include "memory_config.hpp"

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

// Weak declaration for physical frame allocator if present in kernel
extern uintptr_t alloc_frame() noexcept __attribute__((weak));

namespace {

constexpr size_t PAGE_ENTRY_COUNT = 512U;
constexpr uint64_t TABLE_ADDR_MASK = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t DESC_TYPE_TABLE = 0x3ULL;


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

alignas(4096) uint64_t g_ttbr0_l0[512] = {0}; // TTBR0_EL1 (User / Lower VA space)
alignas(4096) uint64_t g_ttbr1_l0[512] = {0}; // TTBR1_EL1 (Kernel / Higher VA space)

uintptr_t allocate_table_page() noexcept {
    uintptr_t phys = 0U;
    if (alloc_frame != nullptr) {
        phys = alloc_frame();
    }

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

    auto* table = reinterpret_cast<uint64_t*>(phys);
    for (size_t i = 0U; i < 512U; ++i) {
        table[i] = 0ULL;
    }
    flush_dcache_range(phys, 4096U);
    return phys;
}

bool map_page_in_root(uint64_t* root_l0, uintptr_t virt_addr, uintptr_t phys_addr,
                      HAL::PageFlags flags) noexcept {
    const size_t l0_idx = (virt_addr >> 39U) & 0x1FFU;
    const size_t l1_idx = (virt_addr >> 30U) & 0x1FFU;
    const size_t l2_idx = (virt_addr >> 21U) & 0x1FFU;
    const size_t l3_idx = (virt_addr >> 12U) & 0x1FFU;

    // L0 -> L1
    if ((root_l0[l0_idx] & 1U) == 0U) {
        uintptr_t l1_paddr = allocate_table_page();
        if (l1_paddr == 0U) {
            return false;
        }
        root_l0[l0_idx] = (l1_paddr & 0x000FFFFFFFFFF000ULL) | 0x3ULL;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&root_l0[l0_idx]), sizeof(uint64_t));
    }
    auto* l1_table = reinterpret_cast<uint64_t*>(root_l0[l0_idx] & 0x000FFFFFFFFFF000ULL);

    // L1 -> L2
    if ((l1_table[l1_idx] & 1U) == 0U) {
        uintptr_t l2_paddr = allocate_table_page();
        if (l2_paddr == 0U) {
            return false;
        }
        l1_table[l1_idx] = (l2_paddr & 0x000FFFFFFFFFF000ULL) | 0x3ULL;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&l1_table[l1_idx]), sizeof(uint64_t));
    }
    auto* l2_table = reinterpret_cast<uint64_t*>(l1_table[l1_idx] & 0x000FFFFFFFFFF000ULL);

    // L2 -> L3
    if ((l2_table[l2_idx] & 1U) == 0U) {
        uintptr_t l3_paddr = allocate_table_page();
        if (l3_paddr == 0U) {
            return false;
        }
        l2_table[l2_idx] = (l3_paddr & 0x000FFFFFFFFFF000ULL) | 0x3ULL;
        flush_dcache_range(reinterpret_cast<uintptr_t>(&l2_table[l2_idx]), sizeof(uint64_t));
    }
    auto* l3_table = reinterpret_cast<uint64_t*>(l2_table[l2_idx] & 0x000FFFFFFFFFF000ULL);

    // L3 Page Descriptor Construction
    uint64_t desc = (phys_addr & 0x000FFFFFFFFFF000ULL) | 0x3ULL; // Valid + Page Descriptor
    desc |= (1ULL << 10U);                                        // Access Flag (AF)

    const bool is_write = HAL::has_flag(flags, HAL::PageFlags::Write);
    const bool is_user = HAL::has_flag(flags, HAL::PageFlags::User);
    const bool is_exec = HAL::has_flag(flags, HAL::PageFlags::Execute);
    const bool is_device = HAL::has_flag(flags, HAL::PageFlags::Device);

    // Select MAIR_EL1 Index: Attr 0 = Device-nGnRnE (0x00), Attr 1 = Normal WB (0xFF)
    const uint64_t attr_idx = is_device ? 0ULL : 1ULL;
    desc |= (attr_idx << 2U);

    // AP[2:1] Access Permissions:
    // 00 = Kernel RW / User None
    // 01 = Kernel RW / User RW
    // 10 = Kernel RO / User None
    // 11 = Kernel RO / User RO
    uint64_t ap_ = 0ULL;
    if (!is_write) {
        ap_ = is_user ? 0x3ULL : 0x2ULL;
    } else {
        ap_ = is_user ? 0x1ULL : 0x0ULL;
    }
    desc |= (ap_ << 6U);

    if (!is_device) {
        desc |= (3ULL << 8U); // Inner Shareable
    }

    // Execute-Never Protection (PXN / UXN):
    if (!is_exec) {
        desc |= (1ULL << 53U); // PXN
        desc |= (1ULL << 54U); // UXN
    } else {
        if (is_user) {
            desc |= (1ULL << 53U); // PXN: User executable code cannot execute in Kernel context
        } else {
            desc |= (1ULL << 54U); // UXN: Kernel executable code cannot execute in User context
        }
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

    if ((root_l0[l0_idx] & 1U) == 0U) {
        return false;
    }
    const auto* l1_table =
        reinterpret_cast<const uint64_t*>(root_l0[l0_idx] & 0x000FFFFFFFFFF000ULL);

    if ((l1_table[l1_idx] & 1U) == 0U) {
        return false;
    }
    const auto* l2_table =
        reinterpret_cast<const uint64_t*>(l1_table[l1_idx] & 0x000FFFFFFFFFF000ULL);

    if ((l2_table[l2_idx] & 1U) == 0U) {
        return false;
    }
    auto* l3_table = reinterpret_cast<uint64_t*>(l2_table[l2_idx] & 0x000FFFFFFFFFF000ULL);

    l3_table[l3_idx] = 0ULL;
    flush_dcache_range(reinterpret_cast<uintptr_t>(&l3_table[l3_idx]), sizeof(uint64_t));

    asm volatile("tlbi vaae1is, %0; dsb ish; isb" ::"r"(virt_addr >> 12U) : "memory");
    return true;
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
        const uintptr_t text_start = reinterpret_cast<uintptr_t>(_text_start);
        const uintptr_t text_end = reinterpret_cast<uintptr_t>(_text_end);
        const uintptr_t rodata_start = reinterpret_cast<uintptr_t>(_rodata_start);
        const uintptr_t rodata_end = reinterpret_cast<uintptr_t>(_rodata_end);
        const uintptr_t data_start = reinterpret_cast<uintptr_t>(_data_start);
        const uintptr_t ram_end = Arch::RAM_BASE + (256UL * 1024UL * 1024UL); // 256 MB platform RAM

        // 1. Identity map UART MMIO register space (0x09000000) as Device-nGnRnE (RW + NX)
        map_page_in_root(g_ttbr0_l0, 0x09000000U, 0x09000000U,
                         HAL::PageFlags::Read | HAL::PageFlags::Write | HAL::PageFlags::Device);

        // 2. Identity map low RAM below kernel text (0x40000000 to text_start) for DTB & boot
        // structures
        for (uintptr_t addr = Arch::RAM_BASE; addr < text_start; addr += Arch::PAGE_SIZE) {
            map_page_in_root(g_ttbr0_l0, addr, addr, HAL::PageFlags::Read | HAL::PageFlags::Write);
        }

        // 3. Identity map Kernel .text (RO + Executable)
        for (uintptr_t addr = text_start; addr < text_end; addr += Arch::PAGE_SIZE) {
            map_page_in_root(g_ttbr0_l0, addr, addr,
                             HAL::PageFlags::Read | HAL::PageFlags::Execute);
        }

        // 4. Identity map Kernel .rodata (RO + NX)
        for (uintptr_t addr = rodata_start; addr < rodata_end; addr += Arch::PAGE_SIZE) {
            map_page_in_root(g_ttbr0_l0, addr, addr, HAL::PageFlags::Read);
        }

        // 5. Identity map Kernel .data, .bss, boot stack, and physical RAM pool up to ram_end (RW +
        // NX)
        for (uintptr_t addr = data_start; addr < ram_end; addr += Arch::PAGE_SIZE) {
            map_page_in_root(g_ttbr0_l0, addr, addr, HAL::PageFlags::Read | HAL::PageFlags::Write);
        }

        // 6. Higher-half kernel space mappings for TTBR1_EL1 (0xFFFF800040000000+)
        const uintptr_t higher_half_offset = 0xFFFF800000000000ULL;
        for (uintptr_t addr = Arch::RAM_BASE; addr < text_start; addr += Arch::PAGE_SIZE) {
            map_page_in_root(g_ttbr1_l0, addr + higher_half_offset, addr,
                             HAL::PageFlags::Read | HAL::PageFlags::Write);
        }
        for (uintptr_t addr = text_start; addr < text_end; addr += Arch::PAGE_SIZE) {
            map_page_in_root(g_ttbr1_l0, addr + higher_half_offset, addr,
                             HAL::PageFlags::Read | HAL::PageFlags::Execute);
        }
        for (uintptr_t addr = rodata_start; addr < rodata_end; addr += Arch::PAGE_SIZE) {
            map_page_in_root(g_ttbr1_l0, addr + higher_half_offset, addr, HAL::PageFlags::Read);
        }
        for (uintptr_t addr = data_start; addr < ram_end; addr += Arch::PAGE_SIZE) {
            map_page_in_root(g_ttbr1_l0, addr + higher_half_offset, addr,
                             HAL::PageFlags::Read | HAL::PageFlags::Write);
        }

        // Activate hardware MMU and system control registers
        Arch::init_mmu(reinterpret_cast<uintptr_t>(g_ttbr0_l0),
                       reinterpret_cast<uintptr_t>(g_ttbr1_l0));
    }

    HAL::MmuStatus status() const noexcept override {
        uint64_t sctlr;
        asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

        return HAL::MmuStatus{.mmu_enabled = (sctlr & (1ULL << 0U)) != 0U,
                              .dcache_enabled = (sctlr & (1ULL << 2U)) != 0U,
                              .icache_enabled = (sctlr & (1ULL << 12U)) != 0U,
                              .raw_control_reg = sctlr};
    }
};

constinit AArch64MemoryControl aarch64_mmu{};

} // namespace

namespace Arch {

void init_mmu(uintptr_t ttbr0_base, uintptr_t ttbr1_base) {
    flush_dcache_range(ttbr0_base, 4096U);
    flush_dcache_range(ttbr1_base, 4096U);

    // 1. Setup MAIR_EL1: Attr0 = 0x00 (Device-nGnRnE), Attr1 = 0xFF (Normal Inner/Outer Write-Back)
    const uint64_t mair = (0x00ULL << 0U) | (0xFFULL << 8U);
    asm volatile("msr mair_el1, %0" ::"r"(mair));

    // 2. Query CPU PARange to prevent Address Size Faults
    uint64_t mmfr0;
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    const uint64_t pa_range = mmfr0 & 0xFU;

    // 3. Setup TCR_EL1: 48-bit VA space (T0SZ=16, T1SZ=16), 4KB Granules (TG0=00b, TG1=10b),
    // IS=11b, Inner/Outer WB
    const uint64_t tcr = (16ULL << 0U) |    // T0SZ  = 16 (48-bit VA for TTBR0)
                         (1ULL << 8U) |     // IRGN0 = 01 (Normal WB WA)
                         (1ULL << 10U) |    // ORGN0 = 01 (Normal WB WA)
                         (3ULL << 12U) |    // SH0   = 11 (Inner Shareable)
                         (0ULL << 14U) |    // TG0   = 00 (4KB granule for TTBR0)
                         (16ULL << 16U) |   // T1SZ  = 16 (48-bit VA for TTBR1)
                         (1ULL << 24U) |    // IRGN1 = 01 (Normal WB WA)
                         (1ULL << 26U) |    // ORGN1 = 01 (Normal WB WA)
                         (3ULL << 28U) |    // SH1   = 11 (Inner Shareable)
                         (2ULL << 30U) |    // TG1   = 10 (4KB granule for TTBR1)
                         (pa_range << 32U); // IPS   = PA physical address range
    asm volatile("msr tcr_el1, %0" ::"r"(tcr));

    // 4. Set Page Table Roots
    asm volatile("msr ttbr0_el1, %0" ::"r"(ttbr0_base));
    asm volatile("msr ttbr1_el1, %0" ::"r"(ttbr1_base));

    // 5. Full system barrier & TLB flush
    asm volatile("dsb sy\n\t"
                 "tlbi vmalle1is\n\t"
                 "dsb sy\n\t"
                 "isb" ::
                     : "memory");

    // 6. Enable MMU (M bit 0), Data Cache (C bit 2), and Instruction Cache (I bit 12)
    uint64_t sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0U);  // M: Enable MMU
    sctlr |= (1ULL << 2U);  // C: Enable Data Cache
    sctlr |= (1ULL << 12U); // I: Enable Instruction Cache
    sctlr &= ~(1ULL << 1U); // Clear strict alignment check (A)
    sctlr &= ~(1ULL << 3U); // Clear Stack Alignment check (SA)
    sctlr &= ~(1ULL << 4U); // Clear Stack Alignment check for EL0 (SA0)
    asm volatile("msr sctlr_el1, %0\n\t"
                 "isb" ::"r"(sctlr)
                 : "memory");
}

} // namespace Arch

namespace HAL {
MemoryControl& get_mmu() noexcept {
    return aarch64_mmu;
}
uintptr_t AddressSpaceBackend::create_root() noexcept {
    const uintptr_t root_phys = allocate_zeroed_table();
    if (root_phys == 0U) {
        return 0U;
    }

    const uintptr_t boot_ttbr0 = get_boot_ttbr0();
    if (boot_ttbr0 != 0U) {
        auto* const new_l0 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(root_phys));
        const auto* const k_l0 = reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(boot_ttbr0));

        const uintptr_t l1_phys = allocate_zeroed_table();
        if (l1_phys == 0U) {
            free_frame(root_phys);
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
            const auto* const k_l2 =
                reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(k_l1[0] & TABLE_ADDR_MASK));

            constexpr size_t UART_L2_IDX = (0x09000000ULL >> 21U) & 0x1FFU;
            new_l2[UART_L2_IDX] = k_l2[UART_L2_IDX];
            flush_dcache_range(reinterpret_cast<uintptr_t>(new_l2), Arch::PAGE_SIZE);
        }
        flush_dcache_range(reinterpret_cast<uintptr_t>(new_l1), Arch::PAGE_SIZE);
    }

    return root_phys;
}

bool AddressSpaceBackend::map_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t paddr, HAL::PageFlags flags) noexcept {
    if (root_paddr == 0U || vaddr >= 0x40000000ULL) {
        return false;
    }
    auto* const root_l0 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(root_paddr));
    return map_page_in_root(root_l0, vaddr, paddr, flags);
}

bool AddressSpaceBackend::unmap_user_page(uintptr_t root_paddr, uintptr_t vaddr) noexcept {
    if (root_paddr == 0U || vaddr >= 0x40000000ULL) {
        return false;
    }
    auto* const root_l0 = reinterpret_cast<const uint64_t*>(Kernel::MM::phys_to_virt(root_paddr));
    return unmap_page_in_root(root_l0, vaddr);
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

        if ((lvl1[0] & 1U) != 0U) {
            const uintptr_t l2_paddr = lvl1[0] & TABLE_ADDR_MASK;
            auto* const lvl2 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(l2_paddr));

            constexpr size_t UART_L2_IDX = (0x09000000ULL >> 21U) & 0x1FFU;
            for (size_t i2 = 0U; i2 < PAGE_ENTRY_COUNT; ++i2) {
                if (i2 != UART_L2_IDX && (lvl2[i2] & 1U) != 0U) {
                    const uintptr_t l3_paddr = lvl2[i2] & TABLE_ADDR_MASK;
                    auto* const lvl3 = reinterpret_cast<uint64_t*>(Kernel::MM::phys_to_virt(l3_paddr));
                    for (size_t i3 = 0U; i3 < PAGE_ENTRY_COUNT; ++i3) {
                        if ((lvl3[i3] & 1U) != 0U) {
                            const uintptr_t page_paddr = lvl3[i3] & TABLE_ADDR_MASK;
                            if (pfa_is_page_allocated(page_paddr)) {
                                free_frame(page_paddr);
                            }
                            lvl3[i3] = 0ULL;
                        }
                    }
                    free_frame(l3_paddr);
                    lvl2[i2] = 0ULL;
                }
            }
            free_frame(l2_paddr);
            lvl1[0] = 0ULL;
        }
        free_frame(l1_paddr);
        lvl0[0] = 0ULL;
    }

    free_frame(root_paddr);
}
} // namespace HAL