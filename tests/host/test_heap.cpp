#include "memory_config.hpp"
#include "mm/address_space.hpp"
#include "mm/heap.hpp"
#include "mm/pfa.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr uintptr_t MOCK_RAM_BASE = 0x40000000ULL;
constexpr size_t MOCK_TOTAL_PAGES = 512U; // 2 MiB mock physical RAM
constexpr size_t MOCK_RAM_BYTES = MOCK_TOTAL_PAGES * Arch::PAGE_SIZE;

alignas(4096) uint8_t g_mock_ram[MOCK_RAM_BYTES]{};
Page g_mock_pages[MOCK_TOTAL_PAGES]{};
size_t g_mock_free_pages = MOCK_TOTAL_PAGES;
bool g_simulate_oom = false;

void reset_mock_pfa() noexcept {
    std::memset(g_mock_ram, 0, sizeof(g_mock_ram));
    for (size_t i = 0U; i < MOCK_TOTAL_PAGES; ++i) {
        g_mock_pages[i] = Page{};
    }
    g_mock_free_pages = MOCK_TOTAL_PAGES;
    g_simulate_oom = false;
    Kernel::MM::heap_init();
}

bool is_aligned(const void* ptr, size_t align) noexcept {
    return (reinterpret_cast<uintptr_t>(ptr) & (align - 1U)) == 0U;
}

} // namespace

// ==========================================================================
// Host Mock PFA & Direct-Mapping Hooks
// ==========================================================================

namespace Kernel::MM {

uintptr_t phys_to_virt(uintptr_t paddr) noexcept {
    if (paddr < MOCK_RAM_BASE || paddr >= (MOCK_RAM_BASE + MOCK_RAM_BYTES)) {
        return 0U;
    }
    return reinterpret_cast<uintptr_t>(g_mock_ram) + (paddr - MOCK_RAM_BASE);
}

uintptr_t virt_to_phys(uintptr_t vaddr) noexcept {
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_mock_ram);
    if (vaddr < base || vaddr >= (base + MOCK_RAM_BYTES)) {
        return 0U;
    }
    return MOCK_RAM_BASE + (vaddr - base);
}

} // namespace Kernel::MM

uintptr_t alloc_frames(size_t count) noexcept {
    if (g_simulate_oom || count == 0U || count > g_mock_free_pages) {
        return 0U;
    }

    for (size_t start = 0U; start + count <= MOCK_TOTAL_PAGES; ++start) {
        bool span_free = true;
        for (size_t offset = 0U; offset < count; ++offset) {
            if (g_mock_pages[start + offset].flags != PageFlags::Free) {
                span_free = false;
                start += offset;
                break;
            }
        }
        if (span_free) {
            for (size_t offset = 0U; offset < count; ++offset) {
                g_mock_pages[start + offset] = Page{};
                g_mock_pages[start + offset].flags = PageFlags::Allocated;
            }
            g_mock_free_pages -= count;
            return MOCK_RAM_BASE + (start * Arch::PAGE_SIZE);
        }
    }

    return 0U;
}

uintptr_t alloc_frame() noexcept {
    return alloc_frames(1U);
}

void free_frames(uintptr_t paddr, size_t count) noexcept {
    assert(count > 0U);
    assert((paddr & (Arch::PAGE_SIZE - 1U)) == 0U);
    assert(paddr >= MOCK_RAM_BASE && paddr < (MOCK_RAM_BASE + MOCK_RAM_BYTES));

    const size_t start = (paddr - MOCK_RAM_BASE) / Arch::PAGE_SIZE;
    assert(start + count <= MOCK_TOTAL_PAGES);

    for (size_t i = 0U; i < count; ++i) {
        assert(g_mock_pages[start + i].flags != PageFlags::Allocated);
        g_mock_pages[start + i] = Page{};
    }
    g_mock_free_pages += count;
}

void free_frame(uintptr_t paddr) noexcept {
    free_frames(paddr, 1U);
}

Page* page_from_paddr(uintptr_t paddr) noexcept {
    if ((paddr & (Arch::PAGE_SIZE - 1U)) != 0U) {
        return nullptr;
    }
    if (paddr < MOCK_RAM_BASE || paddr >= (MOCK_RAM_BASE + MOCK_RAM_BYTES)) {
        return nullptr;
    }
    return &g_mock_pages[(paddr - MOCK_RAM_BASE) / Arch::PAGE_SIZE];
}

uintptr_t page_to_paddr(const Page* page) noexcept {
    assert(page != nullptr);
    assert(page >= &g_mock_pages[0] && page < &g_mock_pages[MOCK_TOTAL_PAGES]);
    const auto idx = static_cast<size_t>(page - &g_mock_pages[0]);
    return MOCK_RAM_BASE + (idx * Arch::PAGE_SIZE);
}

size_t pfa_free_pages_count() noexcept {
    return g_mock_free_pages;
}

size_t pfa_allocated_pages_count() noexcept {
    return MOCK_TOTAL_PAGES - g_mock_free_pages;
}

bool pfa_is_page_allocated(uintptr_t paddr) noexcept {
    const Page* const page = page_from_paddr(paddr & ~(Arch::PAGE_SIZE - 1U));
    return page != nullptr && page->flags != PageFlags::Free;
}

// ==========================================================================
// Host ASan + UBSan Verification Suites
// ==========================================================================

namespace {

void test_slab_classes_and_expansion() {
    reset_mock_pfa();

    constexpr size_t class_sizes[8] = {16U, 32U, 64U, 128U, 256U, 512U, 1024U, 2048U};
    for (size_t c = 0U; c < 8U; ++c) {
        const size_t sz = class_sizes[c];
        const size_t slots_per_page = Arch::PAGE_SIZE / sz;
        const size_t total_objs = (slots_per_page * 3U) + 1U; // Spans 4 slab pages

        std::vector<void*> ptrs;
        ptrs.reserve(total_objs);

        for (size_t i = 0U; i < total_objs; ++i) {
            void* p = Kernel::MM::kmalloc(sz);
            assert(p != nullptr);
            assert(is_aligned(p, sz));
            std::memset(p, static_cast<int>((i + c) & 0xFFU), sz);
            ptrs.push_back(p);
        }

        auto stats = Kernel::MM::heap_get_stats();
        assert(stats.active_slab_objects == total_objs);
        assert(stats.slab_pages == 4U);
        assert(stats.active_bytes == total_objs * sz);

        // Verify payload integrity across all allocated slots
        for (size_t i = 0U; i < total_objs; ++i) {
            const auto* bytes = static_cast<const uint8_t*>(ptrs[i]);
            const auto expected = static_cast<uint8_t>((i + c) & 0xFFU);
            for (size_t b = 0U; b < sz; ++b) {
                assert(bytes[b] == expected);
            }
        }

        for (void* p : ptrs) {
            Kernel::MM::kfree(p);
        }

        // 3 excess empty slab pages must be reclaimed back to PFA; 1 reserve slab retained
        stats = Kernel::MM::heap_get_stats();
        assert(stats.active_slab_objects == 0U);
        assert(stats.active_bytes == 0U);
        assert(stats.slab_pages == 1U);
        assert(pfa_free_pages_count() == MOCK_TOTAL_PAGES - 1U);
    }
}

void test_zero_size_and_null_semantics() {
    reset_mock_pfa();

    // kfree(nullptr) is a no-op
    Kernel::MM::kfree(nullptr);
    assert(pfa_free_pages_count() == MOCK_TOTAL_PAGES);

    // kmalloc(0) returns distinct, valid, 16-byte aligned non-null pointers
    void* p0 = Kernel::MM::kmalloc(0U);
    void* p1 = Kernel::MM::kmalloc(0U);
    assert(p0 != nullptr && p1 != nullptr && p0 != p1);
    assert(is_aligned(p0, 16U));
    assert(is_aligned(p1, 16U));

    // krealloc(nullptr, ...) delegates to kmalloc
    void* p2 = Kernel::MM::krealloc(nullptr, 48U);
    assert(p2 != nullptr && is_aligned(p2, 16U));

    // krealloc(ptr, 0) frees ptr and returns nullptr
    void* p3 = Kernel::MM::krealloc(p2, 0U);
    assert(p3 == nullptr);

    Kernel::MM::kfree(p0);
    Kernel::MM::kfree(p1);
    assert(Kernel::MM::heap_get_stats().active_slab_objects == 0U);
}

void test_compound_allocations() {
    reset_mock_pfa();

    constexpr size_t sizes[] = {4097U, 8192U, 64U * 1024U, 1024U * 1024U};
    for (size_t sz : sizes) {
        const size_t expected_pages = (sz + Arch::PAGE_SIZE - 1U) / Arch::PAGE_SIZE;
        void* ptr = Kernel::MM::kmalloc(sz);
        assert(ptr != nullptr);
        assert(is_aligned(ptr, Arch::PAGE_SIZE));

        const uintptr_t paddr = Kernel::MM::virt_to_phys(reinterpret_cast<uintptr_t>(ptr));
        const Page* head = page_from_paddr(paddr);
        assert(head != nullptr);
        assert(head->flags == PageFlags::Compound);
        assert(head->ref_count == expected_pages);

        auto stats = Kernel::MM::heap_get_stats();
        assert(stats.compound_pages == expected_pages);
        assert(stats.active_bytes == expected_pages * Arch::PAGE_SIZE);

        std::memset(ptr, 0xA5, sz);
        Kernel::MM::kfree(ptr);

        stats = Kernel::MM::heap_get_stats();
        assert(stats.compound_pages == 0U);
        assert(stats.active_bytes == 0U);
        assert(pfa_free_pages_count() == MOCK_TOTAL_PAGES);
    }
}

void test_alignments() {
    reset_mock_pfa();

    constexpr size_t alignments[] = {1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U, 512U, 2048U, 4096U};
    for (size_t align : alignments) {
        void* ptr = Kernel::MM::kmalloc(24U, align);
        assert(ptr != nullptr);
        const size_t min_expected = (align < 16U) ? 16U : align;
        assert(is_aligned(ptr, min_expected));
        Kernel::MM::kfree(ptr);
    }

    // Non-power-of-two alignments must return nullptr
    assert(Kernel::MM::kmalloc(32U, 3U) == nullptr);
    assert(Kernel::MM::kmalloc(32U, 24U) == nullptr);
}

void test_krealloc_transitions() {
    reset_mock_pfa();

    // 1. Start with 256B slab allocation
    auto* ptr = static_cast<uint8_t*>(Kernel::MM::kzalloc(200U));
    assert(ptr != nullptr);
    for (size_t i = 0U; i < 200U; ++i) {
        assert(ptr[i] == 0U);
        ptr[i] = static_cast<uint8_t>((i * 3U + 1U) & 0xFFU);
    }

    // 2. In-place retain within (128, 256] capacity window
    void* same_ptr = Kernel::MM::krealloc(ptr, 180U);
    assert(same_ptr == ptr);

    // 3. Shrink below half capacity (256 -> 32) moves to smaller slab class
    auto* shrunk_ptr = static_cast<uint8_t*>(Kernel::MM::krealloc(ptr, 32U));
    assert(shrunk_ptr != nullptr && shrunk_ptr != ptr);
    for (size_t i = 0U; i < 32U; ++i) {
        assert(shrunk_ptr[i] == static_cast<uint8_t>((i * 3U + 1U) & 0xFFU));
    }

    // 4. Grow across slab classes (32 -> 1024)
    auto* grown_slab = static_cast<uint8_t*>(Kernel::MM::krealloc(shrunk_ptr, 1024U));
    assert(grown_slab != nullptr);
    for (size_t i = 0U; i < 32U; ++i) {
        assert(grown_slab[i] == static_cast<uint8_t>((i * 3U + 1U) & 0xFFU));
    }
    for (size_t i = 0U; i < 1024U; ++i) {
        grown_slab[i] = static_cast<uint8_t>(i & 0xFFU);
    }

    // 5. Promote slab -> compound (1024 -> 12288)
    auto* compound_ptr = static_cast<uint8_t*>(Kernel::MM::krealloc(grown_slab, 12288U));
    assert(compound_ptr != nullptr);
    assert(Kernel::MM::heap_get_stats().compound_pages == 3U);
    for (size_t i = 0U; i < 1024U; ++i) {
        assert(compound_ptr[i] == static_cast<uint8_t>(i & 0xFFU));
    }

    // 6. In-place compound retain within (6144, 12288] window
    void* same_compound = Kernel::MM::krealloc(compound_ptr, 9000U);
    assert(same_compound == compound_ptr);

    // 7. Shrink compound -> slab (12288 -> 128)
    auto* demoted_slab = static_cast<uint8_t*>(Kernel::MM::krealloc(compound_ptr, 128U));
    assert(demoted_slab != nullptr);
    assert(Kernel::MM::heap_get_stats().compound_pages == 0U);
    for (size_t i = 0U; i < 128U; ++i) {
        assert(demoted_slab[i] == static_cast<uint8_t>(i & 0xFFU));
    }

    Kernel::MM::kfree(demoted_slab);
    assert(Kernel::MM::heap_get_stats().active_slab_objects == 0U);
}

void test_oom_graceful_failure() {
    reset_mock_pfa();

    void* keep = Kernel::MM::kmalloc(64U);
    assert(keep != nullptr);
    std::memset(keep, 0x7E, 64U);

    // Simulate PFA exhaustion
    g_simulate_oom = true;
    assert(Kernel::MM::kmalloc(512U) == nullptr);
    assert(Kernel::MM::kmalloc(16384U) == nullptr);
    assert(Kernel::MM::kzalloc(1024U) == nullptr);

    // krealloc OOM must return nullptr while preserving original buffer intact
    assert(Kernel::MM::krealloc(keep, 8192U) == nullptr);
    const auto* bytes = static_cast<const uint8_t*>(keep);
    for (size_t i = 0U; i < 64U; ++i) {
        assert(bytes[i] == 0x7EU);
    }

    g_simulate_oom = false;
    Kernel::MM::kfree(keep);
    assert(Kernel::MM::heap_get_stats().active_slab_objects == 0U);
}

} // namespace

int main() {
    test_slab_classes_and_expansion();
    test_zero_size_and_null_semantics();
    test_compound_allocations();
    test_alignments();
    test_krealloc_transitions();
    test_oom_graceful_failure();

    std::printf("[HOST TEST] test_heap: ALL PASSED\n");
    return 0;
}