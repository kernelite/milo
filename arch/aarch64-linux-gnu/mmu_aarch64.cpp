#include "hal/mmu.hpp"
#include "cpu_context.hpp"
#include "hal/console.hpp"
#include "mm/pfa.hpp"

#include <cstddef>
#include <cstdint>

namespace {

alignas(4096) uint64_t g_ttbr0_l0[512] = {0};
alignas(4096) uint64_t g_ttbr1_l0[512] = {0};

inline void flush_dcache_range(uintptr_t addr, size_t size) noexcept {
    const uintptr_t line_size = 64U;
    const uintptr_t end = addr + size;
    addr &= ~(line_size - 1U);
    for (; addr < end; addr += line_size) {
        asm volatile("dc civac, %0" :: "r"(addr) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

uintptr_t allocate_table_page() noexcept {
    uintptr_t phys = alloc_frame();
    if (phys == 0U) {
        return 0U;
    }
    auto* table = reinterpret_cast<uint64_t*>(phys);
    for (size_t i = 0U; i < 512U; ++i) {
        table[i] = 0ULL;
    }
    flush_dcache_range(phys, 4096U);
    return phys;
}

bool map_page_in_root(uint64_t* root_l0, uintptr_t virt_addr, uintptr_t phys_addr, ::HAL::PageFlags flags) noexcept {
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

    uint64_t desc = (phys_addr & 0x000FFFFFFFFFF000ULL) | 0x3ULL;
    desc |= (1ULL << 10U); // Access Flag (AF)

    const bool is_write  = (flags & ::HAL::PageFlags::Write);
    const bool is_user   = (flags & ::HAL::PageFlags::User);
    const bool is_exec   = (flags & ::HAL::PageFlags::Execute);
    const bool is_device = (flags & ::HAL::PageFlags::Device);

    const uint64_t attr_idx = is_device ? 0ULL : 1ULL;
    desc |= (attr_idx << 2U);

    uint64_t ap_ = 0ULL;
    if (!is_write) {
        ap_ |= 0x2ULL;
    }
    if (is_user) {
        ap_ |= 0x1ULL;
    }
    desc |= (ap_ << 6U);

    if (!is_device) {
        desc |= (3ULL << 8U); // Inner Shareable
    }

    if (!is_exec) {
        desc |= (1ULL << 53U); // PXN
        desc |= (1ULL << 54U); // UXN
    } else {
        if (is_user) {
            desc |= (1ULL << 53U); // PXN
        } else {
            desc |= (1ULL << 54U); // UXN
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
    const auto* l1_table = reinterpret_cast<const uint64_t*>(root_l0[l0_idx] & 0x000FFFFFFFFFF000ULL);

    if ((l1_table[l1_idx] & 1U) == 0U) {
        return false;
    }
    const auto* l2_table = reinterpret_cast<const uint64_t*>(l1_table[l1_idx] & 0x000FFFFFFFFFF000ULL);

    if ((l2_table[l2_idx] & 1U) == 0U) {
        return false;
    }
    auto* l3_table = reinterpret_cast<uint64_t*>(l2_table[l2_idx] & 0x000FFFFFFFFFF000ULL);

    l3_table[l3_idx] = 0ULL;
    flush_dcache_range(reinterpret_cast<uintptr_t>(&l3_table[l3_idx]), sizeof(uint64_t));

    asm volatile("tlbi vaae1is, %0; dsb ish; isb" :: "r"(virt_addr >> 12U) : "memory");
    return true;
}

class AArch64MemoryControl : public ::HAL::MemoryControl {
  public:
    constexpr AArch64MemoryControl() = default;

    bool map(uintptr_t virt_addr, uintptr_t phys_addr, ::HAL::PageFlags flags) override {
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
        Arch::init_mmu(reinterpret_cast<uintptr_t>(g_ttbr0_l0), reinterpret_cast<uintptr_t>(g_ttbr1_l0));
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

    // 1. Setup MAIR_EL1
    const uint64_t mair = (0x00ULL << 0U) | (0xFFULL << 8U);
    asm volatile("msr mair_el1, %0" :: "r"(mair));

    // 2. Query CPU PARange to prevent Address Size Faults
    uint64_t mmfr0;
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    const uint64_t pa_range = mmfr0 & 0xFU;

    // 3. Setup TCR_EL1 with hardware-matched IPS
    const uint64_t tcr = (16ULL << 0U)          |
                         (1ULL  << 8U)          |
                         (1ULL  << 10U)         |
                         (3ULL  << 12U)         |
                         (0ULL  << 14U)         |
                         (16ULL << 16U)         |
                         (1ULL  << 24U)         |
                         (1ULL  << 26U)         |
                         (3ULL  << 28U)         |
                         (2ULL  << 30U)         |
                         (pa_range << 32U);
    asm volatile("msr tcr_el1, %0" :: "r"(tcr));

    // 4. Set Page Table Roots
    asm volatile("msr ttbr0_el1, %0" :: "r"(ttbr0_base));
    asm volatile("msr ttbr1_el1, %0" :: "r"(ttbr1_base));

    // 5. Full system barrier & TLB flush
    asm volatile("dsb sy\n\t"
                 "tlbi vmalle1is\n\t"
                 "dsb sy\n\t"
                 "isb" ::: "memory");

    // 6. Enable MMU and Caches cleanly
    uint64_t sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0U);  // M
    sctlr |= (1ULL << 2U);  // C
    sctlr |= (1ULL << 12U); // I
    sctlr &= ~(1ULL << 1U); // Clear strict alignment check (A)
    sctlr &= ~(1ULL << 3U); // Clear Stack Alignment check (SA)
    sctlr &= ~(1ULL << 4U); // Clear Stack Alignment check for EL0 (SA0)
    asm volatile("msr sctlr_el1, %0\n\t"
                 "isb" :: "r"(sctlr) : "memory");
}

} // namespace Arch

namespace HAL {
MemoryControl& get_mmu() noexcept {
    return aarch64_mmu;
}
} // namespace HAL