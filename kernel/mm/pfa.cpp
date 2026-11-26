#include "mm/pfa.hpp"

#include "hal/console.hpp"
#include "memory_config.hpp"
#include "mm/AddressSpace.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::MM {

namespace {

constexpr size_t MAX_RESERVED_SPANS = 40U;

struct Span {
    uintptr_t start{0U};
    uintptr_t end{0U};
};

constinit PageFrameAllocator g_pfa{};

constexpr uintptr_t align_down_page(uintptr_t addr) noexcept {
    return addr & ~(Arch::AArch64::PAGE_SIZE - 1U);
}

constexpr uintptr_t align_up_page(uintptr_t addr) noexcept {
    return (addr + Arch::AArch64::PAGE_SIZE - 1U) & ~(Arch::AArch64::PAGE_SIZE - 1U);
}

void pfa_print(const char* str) noexcept {
    size_t len = 0U;
    while (str[len] != '\0') {
        ++len;
    }
    HAL::get_console().write(str, len);
}

void pfa_print_hex64(uint64_t val) noexcept {
    char buf[19] = "0x0000000000000000";
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 17; i >= 2; --i) {
        buf[i] = hex_chars[val & 0xFU];
        val >>= 4U;
    }
    pfa_print(buf);
}

void add_reserved_span(Span* spans, size_t& count, uintptr_t start, uintptr_t end) noexcept {
    if (count >= MAX_RESERVED_SPANS || end <= start) {
        return;
    }
    spans[count].start = align_down_page(start);
    spans[count].end = align_up_page(end);
    ++count;
}

uintptr_t find_metadata_carveout(const Kernel::Boot::MemoryMap& mem_map, size_t needed_bytes,
                                 const Span* reserved_spans, size_t reserved_count) noexcept {
    if (needed_bytes == 0U) {
        return 0U;
    }

    for (size_t r_idx = 0U; r_idx < mem_map.count; ++r_idx) {
        const Kernel::Boot::MemoryRegion& region = mem_map.regions[r_idx];
        uintptr_t cand = align_up_page(region.base_paddr);
        const uintptr_t region_end = align_down_page(region.base_paddr + region.size);

        while (cand <= region_end && (region_end - cand) >= needed_bytes) {
            const uintptr_t cand_end = cand + needed_bytes;
            bool collided = false;

            for (size_t s_idx = 0U; s_idx < reserved_count; ++s_idx) {
                const Span& sp = reserved_spans[s_idx];
                if (cand < sp.end && cand_end > sp.start) {
                    const uintptr_t next_cand = align_up_page(sp.end);
                    cand = (next_cand > cand) ? next_cand : (cand + Arch::AArch64::PAGE_SIZE);
                    collided = true;
                    break;
                }
            }

            if (!collided) {
                return cand;
            }
        }
    }

    return 0U;
}

} // namespace

PageFrameAllocator& PageFrameAllocator::instance() noexcept {
    return g_pfa;
}

void PageFrameAllocator::mark_range_reserved(uintptr_t start, uintptr_t end) noexcept {
    if (m_pages == nullptr || end <= start || end <= m_ram_start || start >= m_ram_end) {
        return;
    }

    const uintptr_t clamped_start = (start < m_ram_start) ? m_ram_start : start;
    const uintptr_t clamped_end = (end > m_ram_end) ? m_ram_end : end;
    const uintptr_t aligned_start = align_down_page(clamped_start);
    uintptr_t aligned_end = align_up_page(clamped_end);
    if (aligned_end > m_ram_end) {
        aligned_end = m_ram_end;
    }

    for (uintptr_t paddr = aligned_start; paddr < aligned_end; paddr += Arch::AArch64::PAGE_SIZE) {
        const size_t idx = (paddr - m_ram_start) >> Arch::AArch64::PAGE_SHIFT;
        m_pages[idx].mark_reserved();
    }
}

void PageFrameAllocator::init(const Kernel::Boot::MemoryMap& mem_map, uintptr_t kernel_start,
                              uintptr_t kernel_end, uintptr_t dtb_paddr, size_t dtb_size,
                              uintptr_t initrd_start, uintptr_t initrd_end,
                              const Kernel::Boot::ReserveMap* reserve_map) noexcept {
    m_pages = nullptr;
    m_pages_paddr = 0U;
    m_pages_bytes = 0U;
    m_ram_start = mem_map.min_paddr;
    m_ram_end = mem_map.max_paddr;
    m_total_pages = 0U;
    m_free_pages = 0U;
    m_search_hint = 0U;

    if (mem_map.count == 0U || m_ram_end <= m_ram_start) {
        return;
    }

    m_total_pages = (m_ram_end - m_ram_start) >> Arch::AArch64::PAGE_SHIFT;
    if (m_total_pages == 0U) {
        return;
    }

    const size_t needed_bytes = align_up_page(m_total_pages * sizeof(Page));

    Span reserved_spans[MAX_RESERVED_SPANS]{};
    size_t reserved_count = 0U;

    add_reserved_span(reserved_spans, reserved_count, kernel_start, kernel_end);
    if (dtb_paddr != 0U && dtb_size > 0U) {
        add_reserved_span(reserved_spans, reserved_count, dtb_paddr, dtb_paddr + dtb_size);
    }
    if (initrd_end > initrd_start) {
        add_reserved_span(reserved_spans, reserved_count, initrd_start, initrd_end);
    }
    if (reserve_map != nullptr) {
        for (size_t idx = 0U; idx < reserve_map->count; ++idx) {
            const Kernel::Boot::MemoryRegion& entry = reserve_map->entries[idx];
            if (entry.size > 0U) {
                add_reserved_span(reserved_spans, reserved_count, entry.base_paddr,
                                  entry.base_paddr + entry.size);
            }
        }
    }

    const uintptr_t metadata_paddr =
        find_metadata_carveout(mem_map, needed_bytes, reserved_spans, reserved_count);
    if (metadata_paddr == 0U) {
        m_total_pages = 0U;
        return;
    }

    m_pages_paddr = metadata_paddr;
    m_pages_bytes = needed_bytes;

#if defined(__aarch64__)
    m_pages = reinterpret_cast<Page*>(m_pages_paddr);
#else
    constexpr size_t HOST_MAX_METADATA_BYTES = 16UL * 1024UL * 1024UL; // Up to 4 GiB RAM
    alignas(Page) static uint8_t g_host_pages_storage[HOST_MAX_METADATA_BYTES];
    if (needed_bytes > HOST_MAX_METADATA_BYTES) {
        m_total_pages = 0U;
        return;
    }
    m_pages = reinterpret_cast<Page*>(g_host_pages_storage);
#endif

    // 1. Default all frame descriptors in [m_ram_start, m_ram_end) to Reserved so any
    //    discontiguous holes between /memory banks remain permanently non-allocatable.
    for (size_t idx = 0U; idx < m_total_pages; ++idx) {
        m_pages[idx].mark_reserved();
    }

    // 2. Mark discovered usable physical RAM regions as Free.
    for (size_t r_idx = 0U; r_idx < mem_map.count; ++r_idx) {
        const Kernel::Boot::MemoryRegion& region = mem_map.regions[r_idx];
        const uintptr_t r_start = align_up_page(region.base_paddr);
        const uintptr_t r_end = align_down_page(region.base_paddr + region.size);
        for (uintptr_t paddr = r_start; paddr < r_end; paddr += Arch::AArch64::PAGE_SIZE) {
            if (paddr >= m_ram_start && paddr < m_ram_end) {
                const size_t idx = (paddr - m_ram_start) >> Arch::AArch64::PAGE_SHIFT;
                m_pages[idx].mark_free_init();
            }
        }
    }

    // 3. Re-mark all reserved regions (m_pages metadata, kernel, DTB, initrd, /memreserve/).
    mark_range_reserved(m_pages_paddr, m_pages_paddr + m_pages_bytes);
    for (size_t s_idx = 0U; s_idx < reserved_count; ++s_idx) {
        mark_range_reserved(reserved_spans[s_idx].start, reserved_spans[s_idx].end);
    }

    // 4. Count available free frames.
    size_t free_count = 0U;
    for (size_t idx = 0U; idx < m_total_pages; ++idx) {
        if (m_pages[idx].is_free() && m_pages[idx].ref_count() == 0U) {
            ++free_count;
        }
    }
    __atomic_store_n(&m_free_pages, free_count, __ATOMIC_RELEASE);

    pfa_print("[PFA] Physical Frame Allocator Initialized.\r\n");
    pfa_print("  RAM Range   : ");
    pfa_print_hex64(m_ram_start);
    pfa_print(" - ");
    pfa_print_hex64(m_ram_end);
    pfa_print("\r\n  Kernel Range: ");
    pfa_print_hex64(kernel_start);
    pfa_print(" - ");
    pfa_print_hex64(kernel_end);
    pfa_print("\r\n");
}

void PageFrameAllocator::on_mmu_enabled() noexcept {
    if (m_pages_paddr != 0U) {
        m_pages = reinterpret_cast<Page*>(Kernel::MM::phys_to_virt(m_pages_paddr));
    }
}

Page* PageFrameAllocator::page_from_paddr(uintptr_t paddr) const noexcept {
    if (m_pages == nullptr || paddr < m_ram_start || paddr >= m_ram_end) {
        return nullptr;
    }
    const size_t idx = (paddr - m_ram_start) >> Arch::AArch64::PAGE_SHIFT;
    return &m_pages[idx];
}

uintptr_t PageFrameAllocator::page_to_paddr(const Page* page) const noexcept {
    if (m_pages == nullptr || page == nullptr) {
        return 0U;
    }

    const auto page_addr = reinterpret_cast<uintptr_t>(page);
    const auto base_addr = reinterpret_cast<uintptr_t>(m_pages);
    const auto end_addr = base_addr + (m_total_pages * sizeof(Page));

    if (page_addr < base_addr || page_addr >= end_addr ||
        ((page_addr - base_addr) % sizeof(Page)) != 0U) {
        return 0U;
    }

    const size_t idx = static_cast<size_t>(page - m_pages);
    return m_ram_start + (idx << Arch::AArch64::PAGE_SHIFT);
}

void PageFrameAllocator::page_get(uintptr_t paddr) const noexcept {
    Page* const page = page_from_paddr(paddr);
    if (page != nullptr) {
        page->try_retain();
    }
}

void PageFrameAllocator::page_put(uintptr_t paddr) noexcept {
    Page* const page = page_from_paddr(paddr);
    if (page == nullptr || !page->release_ref()) {
        return;
    }

    __atomic_add_fetch(&m_free_pages, 1U, __ATOMIC_RELAXED);

    const size_t idx = static_cast<size_t>(page - m_pages);
    size_t current_hint = __atomic_load_n(&m_search_hint, __ATOMIC_RELAXED);
    while (idx < current_hint &&
           !__atomic_compare_exchange_n(&m_search_hint, &current_hint, idx, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

uint32_t PageFrameAllocator::page_ref_count(uintptr_t paddr) const noexcept {
    const Page* const page = page_from_paddr(paddr);
    if (page == nullptr) {
        return 0U;
    }
    return page->ref_count();
}

uintptr_t PageFrameAllocator::alloc_frame() noexcept {
    if (m_pages == nullptr || __atomic_load_n(&m_free_pages, __ATOMIC_RELAXED) == 0U ||
        m_total_pages == 0U) {
        pfa_print("[PFA ERROR] Out of physical memory frames!\r\n");
        return 0U;
    }

    const size_t start_hint = __atomic_load_n(&m_search_hint, __ATOMIC_RELAXED);
    for (size_t offset = 0U; offset < m_total_pages; ++offset) {
        const size_t idx = (start_hint + offset) % m_total_pages;
        Page& page = m_pages[idx];

        if (page.is_free() && page.try_claim_free()) {
            __atomic_sub_fetch(&m_free_pages, 1U, __ATOMIC_RELAXED);

            const size_t next_hint = (idx + 1U < m_total_pages) ? (idx + 1U) : 0U;
            __atomic_store_n(&m_search_hint, next_hint, __ATOMIC_RELAXED);
            return m_ram_start + (idx << Arch::AArch64::PAGE_SHIFT);
        }
    }

    pfa_print("[PFA ERROR] Out of physical memory frames!\r\n");
    return 0U;
}

uintptr_t PageFrameAllocator::alloc_frames(size_t count) noexcept {
    if (count == 0U) {
        return 0U;
    }
    if (count == 1U) {
        return alloc_frame();
    }
    if (m_pages == nullptr || __atomic_load_n(&m_free_pages, __ATOMIC_RELAXED) < count ||
        count > m_total_pages) {
        pfa_print("[PFA ERROR] Out of physical memory frames!\r\n");
        return 0U;
    }

    size_t run_start = 0U;
    size_t run_len = 0U;

    for (size_t idx = 0U; idx < m_total_pages; ++idx) {
        if (m_pages[idx].is_free()) {
            if (run_len == 0U) {
                run_start = idx;
            }
            ++run_len;
            if (run_len == count) {
                size_t claimed = 0U;
                while (claimed < count) {
                    if (!m_pages[run_start + claimed].try_claim_free()) {
                        break;
                    }
                    ++claimed;
                }

                if (claimed == count) {
                    __atomic_sub_fetch(&m_free_pages, count, __ATOMIC_RELAXED);
                    const size_t next_hint =
                        (run_start + count < m_total_pages) ? (run_start + count) : 0U;
                    __atomic_store_n(&m_search_hint, next_hint, __ATOMIC_RELAXED);
                    return m_ram_start + (run_start << Arch::AArch64::PAGE_SHIFT);
                }

                // Roll back partial claim if another core raced on a page in the span
                for (size_t rollback = 0U; rollback < claimed; ++rollback) {
                    m_pages[run_start + rollback].rollback_claim();
                }
                run_len = 0U;
            }
        } else {
            run_len = 0U;
        }
    }

    pfa_print("[PFA ERROR] Out of physical memory frames!\r\n");
    return 0U;
}

void PageFrameAllocator::free_frame(uintptr_t paddr) noexcept {
    if ((paddr & (Arch::AArch64::PAGE_SIZE - 1U)) != 0U) {
        pfa_print("[PFA PANIC] Unaligned physical frame address passed to free_frame: ");
        pfa_print_hex64(paddr);
        pfa_print("\r\n");
        return;
    }

    if (m_pages == nullptr || paddr < m_ram_start || paddr >= m_ram_end) {
        pfa_print("[PFA PANIC] Out of bounds address passed to free_frame: ");
        pfa_print_hex64(paddr);
        pfa_print("\r\n");
        return;
    }

    const size_t idx = (paddr - m_ram_start) >> Arch::AArch64::PAGE_SHIFT;
    if (m_pages[idx].is_reserved()) {
        return;
    }

    if (m_pages[idx].is_free() || m_pages[idx].ref_count() == 0U) {
        pfa_print("[PFA WARNING] Double-free detected at physical address: ");
        pfa_print_hex64(paddr);
        pfa_print("\r\n");
        return;
    }

    page_put(paddr);
}

void PageFrameAllocator::free_frames(uintptr_t paddr, size_t count) noexcept {
    for (size_t idx = 0U; idx < count; ++idx) {
        free_frame(paddr + (idx << Arch::AArch64::PAGE_SHIFT));
    }
}

bool PageFrameAllocator::is_page_allocated(uintptr_t paddr) const noexcept {
    if (m_pages == nullptr || (paddr & (Arch::AArch64::PAGE_SIZE - 1U)) != 0U ||
        paddr < m_ram_start || paddr >= m_ram_end) {
        return false;
    }
    const size_t idx = (paddr - m_ram_start) >> Arch::AArch64::PAGE_SHIFT;
    return !m_pages[idx].is_free();
}

size_t PageFrameAllocator::free_pages_count() const noexcept {
    return __atomic_load_n(&m_free_pages, __ATOMIC_RELAXED);
}

size_t PageFrameAllocator::allocated_pages_count() const noexcept {
    const size_t free_count = free_pages_count();
    return (m_total_pages >= free_count) ? (m_total_pages - free_count) : 0U;
}

} // namespace Kernel::MM
