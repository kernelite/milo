#include "mm/pfa.hpp"

#include "hal/console.hpp"
#include "memory_config.hpp"
#include "mm/address_space.hpp"

#include <cstddef>
#include <cstdint>

namespace {

constexpr size_t MAX_RESERVED_SPANS = 40U;

struct Span {
    uintptr_t start{0U};
    uintptr_t end{0U};
};

Page* g_pages = nullptr;
uintptr_t g_pages_paddr = 0U;
size_t g_pages_bytes = 0U;
uintptr_t g_ram_start = 0U;
uintptr_t g_ram_end = 0U;
size_t g_total_pages = 0U;
size_t g_free_pages = 0U;
size_t g_search_hint = 0U;

constexpr uintptr_t align_down_page(uintptr_t addr) noexcept {
    return addr & ~(Arch::PAGE_SIZE - 1U);
}

constexpr uintptr_t align_up_page(uintptr_t addr) noexcept {
    return (addr + Arch::PAGE_SIZE - 1U) & ~(Arch::PAGE_SIZE - 1U);
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

uintptr_t find_metadata_carveout(const FDT::MemoryMap& mem_map, size_t needed_bytes,
                                 const Span* reserved_spans, size_t reserved_count) noexcept {
    if (needed_bytes == 0U) {
        return 0U;
    }

    for (size_t r_idx = 0U; r_idx < mem_map.count; ++r_idx) {
        const FDT::MemoryRegion& region = mem_map.regions[r_idx];
        uintptr_t cand = align_up_page(region.base_paddr);
        const uintptr_t region_end = align_down_page(region.base_paddr + region.size);

        while (cand <= region_end && (region_end - cand) >= needed_bytes) {
            const uintptr_t cand_end = cand + needed_bytes;
            bool collided = false;

            for (size_t s_idx = 0U; s_idx < reserved_count; ++s_idx) {
                const Span& sp = reserved_spans[s_idx];
                if (cand < sp.end && cand_end > sp.start) {
                    const uintptr_t next_cand = align_up_page(sp.end);
                    cand = (next_cand > cand) ? next_cand : (cand + Arch::PAGE_SIZE);
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

void mark_range_reserved(uintptr_t start, uintptr_t end) noexcept {
    if (g_pages == nullptr || end <= start || end <= g_ram_start || start >= g_ram_end) {
        return;
    }

    const uintptr_t clamped_start = (start < g_ram_start) ? g_ram_start : start;
    const uintptr_t clamped_end = (end > g_ram_end) ? g_ram_end : end;
    const uintptr_t aligned_start = align_down_page(clamped_start);
    uintptr_t aligned_end = align_up_page(clamped_end);
    if (aligned_end > g_ram_end) {
        aligned_end = g_ram_end;
    }

    for (uintptr_t paddr = aligned_start; paddr < aligned_end; paddr += Arch::PAGE_SIZE) {
        const size_t idx = (paddr - g_ram_start) >> Arch::PAGE_SHIFT;
        g_pages[idx].ref_count = 1U;
        g_pages[idx].flags = PageFlags::Reserved;
        g_pages[idx].order = 0U;
        g_pages[idx].slab_cache = nullptr;
    }
}

} // namespace

void pfa_init(const FDT::MemoryMap& mem_map, uintptr_t kernel_start, uintptr_t kernel_end,
              uintptr_t dtb_paddr, size_t dtb_size, uintptr_t initrd_start, uintptr_t initrd_end,
              const FDT::ReserveMap* reserve_map) noexcept {
    g_pages = nullptr;
    g_pages_paddr = 0U;
    g_pages_bytes = 0U;
    g_ram_start = mem_map.min_paddr;
    g_ram_end = mem_map.max_paddr;
    g_total_pages = 0U;
    g_free_pages = 0U;
    g_search_hint = 0U;

    if (mem_map.count == 0U || g_ram_end <= g_ram_start) {
        return;
    }

    g_total_pages = (g_ram_end - g_ram_start) >> Arch::PAGE_SHIFT;
    if (g_total_pages == 0U) {
        return;
    }

    const size_t needed_bytes = align_up_page(g_total_pages * sizeof(Page));

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
            const FDT::MemoryRegion& entry = reserve_map->entries[idx];
            if (entry.size > 0U) {
                add_reserved_span(reserved_spans, reserved_count, entry.base_paddr,
                                  entry.base_paddr + entry.size);
            }
        }
    }

    const uintptr_t metadata_paddr =
        find_metadata_carveout(mem_map, needed_bytes, reserved_spans, reserved_count);
    if (metadata_paddr == 0U) {
        g_total_pages = 0U;
        return;
    }

    g_pages_paddr = metadata_paddr;
    g_pages_bytes = needed_bytes;

#if defined(__aarch64__)
    g_pages = reinterpret_cast<Page*>(g_pages_paddr);
#else
    // Host unit tests pass simulated physical addresses (e.g., 0x40000000) that are not
    // directly mapped in the host process's virtual address space.
    constexpr size_t HOST_MAX_METADATA_BYTES = 16UL * 1024UL * 1024UL; // Up to 4 GiB RAM
    alignas(Page) static uint8_t g_host_pages_storage[HOST_MAX_METADATA_BYTES];
    if (needed_bytes > HOST_MAX_METADATA_BYTES) {
        g_total_pages = 0U;
        return;
    }
    g_pages = reinterpret_cast<Page*>(g_host_pages_storage);
#endif

    // 1. Default all frame descriptors in [g_ram_start, g_ram_end) to Reserved so any
    //    discontiguous holes between /memory banks remain permanently non-allocatable.
    for (size_t idx = 0U; idx < g_total_pages; ++idx) {
        g_pages[idx].ref_count = 1U;
        g_pages[idx].flags = PageFlags::Reserved;
        g_pages[idx].order = 0U;
        g_pages[idx].slab_cache = nullptr;
    }

    // 2. Mark discovered usable physical RAM regions as Free.
    for (size_t r_idx = 0U; r_idx < mem_map.count; ++r_idx) {
        const FDT::MemoryRegion& region = mem_map.regions[r_idx];
        const uintptr_t r_start = align_up_page(region.base_paddr);
        const uintptr_t r_end = align_down_page(region.base_paddr + region.size);
        for (uintptr_t paddr = r_start; paddr < r_end; paddr += Arch::PAGE_SIZE) {
            if (paddr >= g_ram_start && paddr < g_ram_end) {
                const size_t idx = (paddr - g_ram_start) >> Arch::PAGE_SHIFT;
                g_pages[idx].ref_count = 0U;
                g_pages[idx].flags = PageFlags::Free;
            }
        }
    }

    // 3. Re-mark all reserved regions (g_pages metadata, kernel, DTB, initrd, /memreserve/).
    mark_range_reserved(g_pages_paddr, g_pages_paddr + g_pages_bytes);
    for (size_t s_idx = 0U; s_idx < reserved_count; ++s_idx) {
        mark_range_reserved(reserved_spans[s_idx].start, reserved_spans[s_idx].end);
    }

    // 4. Count available free frames.
    size_t free_count = 0U;
    for (size_t idx = 0U; idx < g_total_pages; ++idx) {
        if (g_pages[idx].flags == PageFlags::Free && g_pages[idx].ref_count == 0U) {
            ++free_count;
        }
    }
    g_free_pages = free_count;

    pfa_print("[PFA] Physical Frame Allocator Initialized.\r\n");
    pfa_print("  RAM Range   : ");
    pfa_print_hex64(g_ram_start);
    pfa_print(" - ");
    pfa_print_hex64(g_ram_end);
    pfa_print("\r\n  Kernel Range: ");
    pfa_print_hex64(kernel_start);
    pfa_print(" - ");
    pfa_print_hex64(kernel_end);
    pfa_print("\r\n");
}

void pfa_init(uintptr_t ram_start, size_t ram_size, uintptr_t kernel_start, uintptr_t kernel_end,
              uintptr_t initrd_start, uintptr_t initrd_end) noexcept {
    FDT::MemoryMap mem_map{};
    if (ram_size > 0U) {
        mem_map.regions[0] = FDT::MemoryRegion{.base_paddr = ram_start, .size = ram_size};
        mem_map.count = 1U;
        mem_map.min_paddr = ram_start;
        mem_map.max_paddr = ram_start + ram_size;
        mem_map.total_usable_bytes = ram_size;
    }
    pfa_init(mem_map, kernel_start, kernel_end, 0U, 0U, initrd_start, initrd_end, nullptr);
}

void pfa_on_mmu_enabled() noexcept {
    if (g_pages_paddr != 0U) {
        g_pages = reinterpret_cast<Page*>(Kernel::MM::phys_to_virt(g_pages_paddr));
    }
}

Page* page_from_paddr(uintptr_t paddr) noexcept {
    if (g_pages == nullptr || paddr < g_ram_start || paddr >= g_ram_end) {
        return nullptr;
    }
    const size_t idx = (paddr - g_ram_start) >> Arch::PAGE_SHIFT;
    return &g_pages[idx];
}

uintptr_t page_to_paddr(const Page* page) noexcept {
    if (g_pages == nullptr || page == nullptr) {
        return 0U;
    }

    const auto page_addr = reinterpret_cast<uintptr_t>(page);
    const auto base_addr = reinterpret_cast<uintptr_t>(g_pages);
    const auto end_addr = base_addr + (g_total_pages * sizeof(Page));

    if (page_addr < base_addr || page_addr >= end_addr ||
        ((page_addr - base_addr) % sizeof(Page)) != 0U) {
        return 0U;
    }

    const size_t idx = static_cast<size_t>(page - g_pages);
    return g_ram_start + (idx << Arch::PAGE_SHIFT);
}

void page_get(uintptr_t paddr) noexcept {
    Page* const page = page_from_paddr(paddr);
    if (page == nullptr || has_page_flag(page->flags, PageFlags::Reserved)) {
        return;
    }

    if (page->ref_count < UINT32_MAX) {
        ++page->ref_count;
    }

    if (page->flags == PageFlags::Free) {
        page->flags = PageFlags::Allocated;
        if (g_free_pages > 0U) {
            --g_free_pages;
        }
    }
}

void page_put(uintptr_t paddr) noexcept {
    Page* const page = page_from_paddr(paddr);
    if (page == nullptr || has_page_flag(page->flags, PageFlags::Reserved) ||
        page->ref_count == 0U) {
        return;
    }

    --page->ref_count;
    if (page->ref_count == 0U) {
        page->flags = PageFlags::Free;
        page->order = 0U;
        page->slab_cache = nullptr;
        ++g_free_pages;

        const size_t idx = static_cast<size_t>(page - g_pages);
        if (idx < g_search_hint) {
            g_search_hint = idx;
        }
    }
}

uintptr_t alloc_frame() noexcept {
    if (g_pages == nullptr || g_free_pages == 0U || g_total_pages == 0U) {
        pfa_print("[PFA ERROR] Out of physical memory frames!\r\n");
        return 0U;
    }

    for (size_t offset = 0U; offset < g_total_pages; ++offset) {
        const size_t idx = (g_search_hint + offset) % g_total_pages;
        if (g_pages[idx].flags == PageFlags::Free && g_pages[idx].ref_count == 0U) {
            g_pages[idx].flags = PageFlags::Allocated;
            g_pages[idx].ref_count = 1U;
            g_pages[idx].order = 0U;
            g_pages[idx].slab_cache = nullptr;
            --g_free_pages;
            g_search_hint = (idx + 1U < g_total_pages) ? (idx + 1U) : 0U;
            return g_ram_start + (idx << Arch::PAGE_SHIFT);
        }
    }

    pfa_print("[PFA ERROR] Out of physical memory frames!\r\n");
    return 0U;
}

uintptr_t alloc_frames(size_t count) noexcept {
    if (count == 0U) {
        return 0U;
    }
    if (count == 1U) {
        return alloc_frame();
    }
    if (g_pages == nullptr || g_free_pages < count || count > g_total_pages) {
        pfa_print("[PFA ERROR] Out of physical memory frames!\r\n");
        return 0U;
    }

    size_t run_start = 0U;
    size_t run_len = 0U;

    for (size_t idx = 0U; idx < g_total_pages; ++idx) {
        if (g_pages[idx].flags == PageFlags::Free && g_pages[idx].ref_count == 0U) {
            if (run_len == 0U) {
                run_start = idx;
            }
            ++run_len;
            if (run_len == count) {
                for (size_t k = run_start; k < run_start + count; ++k) {
                    g_pages[k].flags = PageFlags::Allocated;
                    g_pages[k].ref_count = 1U;
                    g_pages[k].order = 0U;
                    g_pages[k].slab_cache = nullptr;
                }
                g_free_pages -= count;
                g_search_hint = (run_start + count < g_total_pages) ? (run_start + count) : 0U;
                return g_ram_start + (run_start << Arch::PAGE_SHIFT);
            }
        } else {
            run_len = 0U;
        }
    }

    pfa_print("[PFA ERROR] Out of physical memory frames!\r\n");
    return 0U;
}

void free_frame(uintptr_t paddr) noexcept {
    if ((paddr & (Arch::PAGE_SIZE - 1U)) != 0U) {
        pfa_print("[PFA PANIC] Unaligned physical frame address passed to free_frame: ");
        pfa_print_hex64(paddr);
        pfa_print("\r\n");
        return;
    }

    if (g_pages == nullptr || paddr < g_ram_start || paddr >= g_ram_end) {
        pfa_print("[PFA PANIC] Out of bounds address passed to free_frame: ");
        pfa_print_hex64(paddr);
        pfa_print("\r\n");
        return;
    }

    const size_t idx = (paddr - g_ram_start) >> Arch::PAGE_SHIFT;
    if (has_page_flag(g_pages[idx].flags, PageFlags::Reserved)) {
        return;
    }

    if (g_pages[idx].flags == PageFlags::Free || g_pages[idx].ref_count == 0U) {
        pfa_print("[PFA WARNING] Double-free detected at physical address: ");
        pfa_print_hex64(paddr);
        pfa_print("\r\n");
        return;
    }

    page_put(paddr);
}

void free_frames(uintptr_t paddr, size_t count) noexcept {
    for (size_t idx = 0U; idx < count; ++idx) {
        free_frame(paddr + (idx << Arch::PAGE_SHIFT));
    }
}

bool pfa_is_page_allocated(uintptr_t paddr) noexcept {
    if (g_pages == nullptr || (paddr & (Arch::PAGE_SIZE - 1U)) != 0U || paddr < g_ram_start ||
        paddr >= g_ram_end) {
        return false;
    }
    const size_t idx = (paddr - g_ram_start) >> Arch::PAGE_SHIFT;
    return g_pages[idx].flags != PageFlags::Free;
}

size_t pfa_free_pages_count() noexcept {
    return g_free_pages;
}

size_t pfa_total_pages_count() noexcept {
    return g_total_pages;
}

uintptr_t pfa_metadata_paddr() noexcept {
    return g_pages_paddr;
}

size_t pfa_metadata_size() noexcept {
    return g_pages_bytes;
}

size_t pfa_allocated_pages_count() noexcept {
    return (g_total_pages >= g_free_pages) ? (g_total_pages - g_free_pages) : 0U;
}
