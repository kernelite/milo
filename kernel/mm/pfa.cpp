#include "mm/pfa.hpp"
#include "hal/console.hpp"

namespace {

alignas(8) uint8_t g_bitmap[BITMAP_SIZE_BYTES];

uintptr_t g_ram_start = 0;
uintptr_t g_ram_end = 0;
size_t g_total_pages = 0;

void print_str(const char* str) noexcept {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    HAL::get_console().write(str, len);
}

void print_hex64(uint64_t val) noexcept {
    char buf[19] = "0x0000000000000000";
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 17; i >= 2; --i) {
        buf[i] = hex_chars[val & 0xFU];
        val >>= 4U;
    }
    print_str(buf);
}

inline void set_bit(size_t page_idx) noexcept {
    g_bitmap[page_idx / 8U] |= static_cast<uint8_t>(1U << (page_idx % 8U));
}

inline void clear_bit(size_t page_idx) noexcept {
    g_bitmap[page_idx / 8U] &= static_cast<uint8_t>(~(1U << (page_idx % 8U)));
}

inline bool test_bit(size_t page_idx) noexcept {
    return (g_bitmap[page_idx / 8U] & static_cast<uint8_t>(1U << (page_idx % 8U))) != 0U;
}

void reserve_range(uintptr_t start_paddr, uintptr_t end_paddr) noexcept {
    if (start_paddr < g_ram_start) {
        start_paddr = g_ram_start;
    }
    if (end_paddr > g_ram_end) {
        end_paddr = g_ram_end;
    }

    size_t start_idx = (start_paddr - RAM_BASE) >> PAGE_SHIFT;
    size_t end_idx = (end_paddr - RAM_BASE + PAGE_SIZE - 1U) >> PAGE_SHIFT;

    for (size_t i = start_idx; i < end_idx && i < g_total_pages; ++i) {
        set_bit(i);
    }
}

} // namespace

void pfa_init(uintptr_t ram_start, size_t ram_size) noexcept {
    g_ram_start = ram_start;
    g_ram_end = ram_start + ram_size;
    g_total_pages = ram_size >> PAGE_SHIFT;

    for (size_t i = 0; i < BITMAP_SIZE_BYTES; ++i) {
        g_bitmap[i] = 0U;
    }

    if (ram_size < MAX_RAM_SIZE) {
        for (size_t i = g_total_pages; i < MAX_PAGES; ++i) {
            set_bit(i);
        }
    }

    const auto kernel_start = reinterpret_cast<uintptr_t>(_text_start);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto kernel_end = reinterpret_cast<uintptr_t>(__bss_end);
    reserve_range(kernel_start, kernel_end);

    const auto bitmap_start = reinterpret_cast<uintptr_t>(&g_bitmap[0]);
    const auto bitmap_end = bitmap_start + sizeof(g_bitmap);
    reserve_range(bitmap_start, bitmap_end);

    print_str("[PFA] Physical Frame Allocator Initialized.\r\n");
    print_str("      RAM Range   : ");
    print_hex64(g_ram_start);
    print_str(" - ");
    print_hex64(g_ram_end);
    print_str("\r\n");
    print_str("      Kernel Range: ");
    print_hex64(kernel_start);
    print_str(" - ");
    print_hex64(kernel_end);
    print_str("\r\n");
}

uintptr_t alloc_frame() noexcept {
    return alloc_frames(1U);
}

uintptr_t alloc_frames(size_t count) noexcept {
    if (count == 0U) {
        return 0U;
    }

    static size_t search_start = 0U;
    size_t consecutive = 0U;
    size_t start_idx = 0U;

    for (size_t scan = 0U; scan < g_total_pages; ++scan) {
        size_t idx = (search_start + scan) % g_total_pages;
        if (!test_bit(idx)) {
            if (consecutive == 0U) {
                start_idx = idx;
            }
            consecutive++;
            if (consecutive == count) {
                for (size_t j = start_idx; j < start_idx + count; ++j) {
                    set_bit(j);
                }
                search_start = (start_idx + count) % g_total_pages;
                return RAM_BASE + (start_idx << PAGE_SHIFT);
            }
        } else {
            consecutive = 0U;
        }
    }

    print_str("[PFA ERROR] Out of physical memory frames!\r\n");
    return 0U;
}

void free_frame(uintptr_t paddr) noexcept {
    free_frames(paddr, 1U);
}

void free_frames(uintptr_t paddr, size_t count) noexcept {
    if ((paddr & (PAGE_SIZE - 1U)) != 0U) {
        print_str("[PFA PANIC] Unaligned physical frame address passed to free_frame: ");
        print_hex64(paddr);
        print_str("\r\n");
        return;
    }

    if (paddr < g_ram_start || paddr >= g_ram_end) {
        print_str("[PFA PANIC] Out of bounds address passed to free_frame: ");
        print_hex64(paddr);
        print_str("\r\n");
        return;
    }

    const size_t start_idx = (paddr - RAM_BASE) >> PAGE_SHIFT;

    for (size_t i = 0U; i < count; ++i) {
        const size_t page_idx = start_idx + i;
        if (!test_bit(page_idx)) {
            print_str("[PFA WARNING] Double-free detected at physical address: ");
            print_hex64(RAM_BASE + (page_idx << PAGE_SHIFT));
            print_str("\r\n");
            continue;
        }
        clear_bit(page_idx);
    }
}