#include "boot/fdt.hpp"
#include "memory_config.hpp"
#include "mm/pfa.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

TEST_CASE("PFA", "Dynamic >512 MiB Metadata Carving, Discontiguous Holes & Reservations") {
    constexpr size_t SIM_SPAN = 1024UL * 1024UL * 1024UL; // 1 GiB (> 512 MiB cap)
    void* const arena = mmap(nullptr, SIM_SPAN, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    EXPECT_TRUE(arena != MAP_FAILED);

    const auto base = reinterpret_cast<uintptr_t>(arena);
    FDT::MemoryMap map{};
    map.regions[0] = FDT::MemoryRegion{.base_paddr = base, .size = 256UL * 1024UL * 1024UL};
    map.regions[1] = FDT::MemoryRegion{.base_paddr = base + (512UL * 1024UL * 1024UL),
                                       .size = 512UL * 1024UL * 1024UL};
    map.count = 2U;
    map.min_paddr = base;
    map.max_paddr = base + SIM_SPAN;
    map.total_usable_bytes = 768UL * 1024UL * 1024UL;

    const uintptr_t kernel_start = base + 0x80000ULL;
    const uintptr_t kernel_end = base + 0x180000ULL;
    const uintptr_t dtb_paddr = base;
    const size_t dtb_size = 0x2000ULL;
    const uintptr_t initrd_start = base + 0x4000000ULL;
    const uintptr_t initrd_end = base + 0x4200000ULL;

    FDT::ReserveMap rsv{};
    rsv.entries[0] = FDT::MemoryRegion{.base_paddr = base + 0x2000000ULL, .size = 0x10000ULL};
    rsv.count = 1U;

    pfa_init(map, kernel_start, kernel_end, dtb_paddr, dtb_size, initrd_start, initrd_end, &rsv);

    EXPECT_EQ(pfa_total_pages_count(), SIM_SPAN / Arch::PAGE_SIZE);
    EXPECT_EQ(pfa_metadata_size(), static_cast<size_t>(262144U * sizeof(Page)));

    const uintptr_t hole_probe = base + (300UL * 1024UL * 1024UL);
    Page* const hole_page = page_from_paddr(hole_probe);
    EXPECT_TRUE(hole_page != nullptr);
    EXPECT_EQ(hole_page->flags, PageFlags::Reserved);
    EXPECT_EQ(hole_page->ref_count, 1U);
    EXPECT_TRUE(pfa_is_page_allocated(hole_probe));

    EXPECT_EQ(page_from_paddr(kernel_start)->flags, PageFlags::Reserved);
    EXPECT_EQ(page_from_paddr(initrd_start)->flags, PageFlags::Reserved);
    EXPECT_EQ(page_from_paddr(rsv.entries[0].base_paddr)->flags, PageFlags::Reserved);
    EXPECT_EQ(page_from_paddr(pfa_metadata_paddr())->flags, PageFlags::Reserved);

    munmap(arena, SIM_SPAN);
}

TEST_CASE("PFA", "Frame Allocation, Reference Counting (page_get/page_put) & Multi-Frame Runs") {
    constexpr size_t ARENA_SIZE = 16UL * 1024UL * 1024UL; // 16 MiB
    void* const arena =
        mmap(nullptr, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_TRUE(arena != MAP_FAILED);

    const auto base = reinterpret_cast<uintptr_t>(arena);
    FDT::MemoryMap map{};
    map.regions[0] = FDT::MemoryRegion{.base_paddr = base, .size = ARENA_SIZE};
    map.count = 1U;
    map.min_paddr = base;
    map.max_paddr = base + ARENA_SIZE;
    map.total_usable_bytes = ARENA_SIZE;

    pfa_init(map, base + 0x10000ULL, base + 0x20000ULL);
    const size_t baseline_free = pfa_free_pages_count();
    EXPECT_TRUE(baseline_free > 0U);

    const uintptr_t f1 = alloc_frame();
    EXPECT_TRUE(f1 != 0U);
    EXPECT_EQ(f1 & (Arch::PAGE_SIZE - 1U), static_cast<uintptr_t>(0U));
    EXPECT_EQ(pfa_free_pages_count(), baseline_free - 1U);

    Page* const p1 = page_from_paddr(f1);
    EXPECT_TRUE(p1 != nullptr);
    EXPECT_EQ(page_to_paddr(p1), f1);
    EXPECT_EQ(p1->flags, PageFlags::Allocated);
    EXPECT_EQ(p1->ref_count, 1U);

    page_get(f1);
    EXPECT_EQ(p1->ref_count, 2U);

    free_frame(f1);
    EXPECT_EQ(p1->ref_count, 1U);
    EXPECT_EQ(p1->flags, PageFlags::Allocated);
    EXPECT_EQ(pfa_free_pages_count(), baseline_free - 1U);

    free_frame(f1);
    EXPECT_EQ(p1->ref_count, 0U);
    EXPECT_EQ(p1->flags, PageFlags::Free);
    EXPECT_EQ(pfa_free_pages_count(), baseline_free);

    const uintptr_t blk = alloc_frames(8U);
    EXPECT_TRUE(blk != 0U);
    EXPECT_EQ(pfa_free_pages_count(), baseline_free - 8U);
    free_frames(blk, 8U);
    EXPECT_EQ(pfa_free_pages_count(), baseline_free);

    munmap(arena, ARENA_SIZE);
}