#include "memory_config.hpp"
#include "mm/pfa.hpp"
#include "mocks/host_hal.hpp"

#include <snitch/snitch.hpp>

namespace {

constexpr uintptr_t RAM_BASE = HostTest::SIM_RAM_BASE;
constexpr size_t RAM_SIZE = 2UL * 1024UL * 1024UL; // 2 MiB (512 pages)
constexpr uintptr_t KERNEL_START = RAM_BASE;
constexpr uintptr_t KERNEL_END = RAM_BASE + (64UL * 1024UL); // 16 pages
constexpr uintptr_t INITRD_START = RAM_BASE + (512UL * 1024UL);
constexpr uintptr_t INITRD_END = RAM_BASE + (576UL * 1024UL); // 16 pages

Kernel::Boot::MemoryMap make_single_region_map(uintptr_t base, size_t size) noexcept {
    Kernel::Boot::MemoryMap map{};
    map.regions[0] = Kernel::Boot::MemoryRegion{.base_paddr = base, .size = size};
    map.count = 1U;
    map.min_paddr = base;
    map.max_paddr = base + size;
    map.total_usable_bytes = size;
    return map;
}

void init_standard_test_pfa() noexcept {
    HostTest::reset_host_hal();
    const Kernel::Boot::MemoryMap map = make_single_region_map(RAM_BASE, RAM_SIZE);
    pfa_init(map, KERNEL_START, KERNEL_END, 0U, 0U, INITRD_START, INITRD_END, nullptr);
}

} // namespace

TEST_CASE("PFA reserves kernel, initrd, metadata, and /memreserve/ spans", "[pfa][mm]") {
    HostTest::reset_host_hal();

    const Kernel::Boot::MemoryMap map = make_single_region_map(RAM_BASE, RAM_SIZE);
    constexpr uintptr_t DTB_START = RAM_BASE + (128UL * 1024UL);
    constexpr size_t DTB_SIZE = 2U * Arch::PAGE_SIZE;
    constexpr uintptr_t MEMRES_START = RAM_BASE + (256UL * 1024UL);
    constexpr size_t MEMRES_SIZE = 4U * Arch::PAGE_SIZE;

    Kernel::Boot::ReserveMap reserve_map{};
    reserve_map.entries[0] =
        Kernel::Boot::MemoryRegion{.base_paddr = MEMRES_START, .size = MEMRES_SIZE};
    reserve_map.count = 1U;

    pfa_init(map, KERNEL_START, KERNEL_END, DTB_START, DTB_SIZE, INITRD_START, INITRD_END,
             &reserve_map);

    SECTION("Reserved regions are marked non-free and excluded from allocations") {
        for (uintptr_t addr = KERNEL_START; addr < KERNEL_END; addr += Arch::PAGE_SIZE) {
            CHECK(pfa_is_page_allocated(addr));
        }
        for (uintptr_t addr = DTB_START; addr < DTB_START + DTB_SIZE; addr += Arch::PAGE_SIZE) {
            CHECK(pfa_is_page_allocated(addr));
        }
        for (uintptr_t addr = INITRD_START; addr < INITRD_END; addr += Arch::PAGE_SIZE) {
            CHECK(pfa_is_page_allocated(addr));
        }
        for (uintptr_t addr = MEMRES_START; addr < MEMRES_START + MEMRES_SIZE;
             addr += Arch::PAGE_SIZE) {
            CHECK(pfa_is_page_allocated(addr));
        }

        uintptr_t frames[64]{};
        size_t count = 0U;
        for (size_t idx = 0U; idx < 64U; ++idx) {
            const uintptr_t paddr = alloc_frame();
            REQUIRE(paddr != 0U);
            REQUIRE((paddr % Arch::PAGE_SIZE) == 0U);
            frames[count++] = paddr;

            const bool overlaps_kernel = (paddr >= KERNEL_START && paddr < KERNEL_END);
            const bool overlaps_dtb = (paddr >= DTB_START && paddr < DTB_START + DTB_SIZE);
            const bool overlaps_initrd = (paddr >= INITRD_START && paddr < INITRD_END);
            const bool overlaps_memres =
                (paddr >= MEMRES_START && paddr < MEMRES_START + MEMRES_SIZE);

            REQUIRE_FALSE(overlaps_kernel);
            REQUIRE_FALSE(overlaps_dtb);
            REQUIRE_FALSE(overlaps_initrd);
            REQUIRE_FALSE(overlaps_memres);
        }

        for (size_t idx = 0U; idx < count; ++idx) {
            free_frame(frames[idx]);
        }
    }

    SECTION("Discontiguous /memory banks keep gaps permanently reserved") {
        HostTest::reset_host_hal();
        Kernel::Boot::MemoryMap split_map{};
        split_map.regions[0] = Kernel::Boot::MemoryRegion{
            .base_paddr = RAM_BASE,
            .size = 256UL * 1024UL,
        };
        split_map.regions[1] = Kernel::Boot::MemoryRegion{
            .base_paddr = RAM_BASE + (512UL * 1024UL),
            .size = 256UL * 1024UL,
        };
        split_map.count = 2U;
        split_map.min_paddr = RAM_BASE;
        split_map.max_paddr = RAM_BASE + (768UL * 1024UL);
        split_map.total_usable_bytes = 512UL * 1024UL;

        pfa_init(split_map, KERNEL_START, KERNEL_END, 0U, 0U, 0U, 0U, nullptr);

        // Hole between 256 KiB and 512 KiB must remain reserved and never handed out
        for (uintptr_t hole_addr = RAM_BASE + (256UL * 1024UL);
             hole_addr < RAM_BASE + (512UL * 1024UL); hole_addr += Arch::PAGE_SIZE) {
            CHECK(pfa_is_page_allocated(hole_addr));
        }

        while (true) {
            const uintptr_t paddr = alloc_frame();
            if (paddr == 0U) {
                break;
            }
            const bool in_hole =
                (paddr >= RAM_BASE + (256UL * 1024UL)) && (paddr < RAM_BASE + (512UL * 1024UL));
            REQUIRE_FALSE(in_hole);
        }
    }
}

TEST_CASE("PFA single-frame, multi-frame, and recycling invariants", "[pfa][mm]") {
    init_standard_test_pfa();
    const size_t baseline_free = pfa_free_pages_count();
    const size_t baseline_alloc = pfa_allocated_pages_count();
    REQUIRE(baseline_free > 32U);

    SECTION("Single frame allocation, page_from_paddr round-trip, and recycling") {
        const uintptr_t frame = alloc_frame();
        REQUIRE(frame != 0U);
        CHECK((frame & (Arch::PAGE_SIZE - 1U)) == 0U);
        CHECK(pfa_is_page_allocated(frame));
        CHECK(pfa_free_pages_count() == baseline_free - 1U);
        CHECK(pfa_allocated_pages_count() == baseline_alloc + 1U);

        Page* const desc = page_from_paddr(frame);
        REQUIRE(desc != nullptr);
        CHECK(page_to_paddr(desc) == frame);
        CHECK(desc->ref_count == 1U);
        CHECK(desc->flags == PageFlags::Allocated);

        free_frame(frame);
        CHECK_FALSE(pfa_is_page_allocated(frame));
        CHECK(pfa_free_pages_count() == baseline_free);

        const uintptr_t recycled = alloc_frame();
        CHECK(recycled == frame);
        free_frame(recycled);
        CHECK(pfa_free_pages_count() == baseline_free);
    }

    SECTION("Contiguous multi-frame allocation and partial-run skipping") {
        const uintptr_t frame0 = alloc_frame();
        const uintptr_t frame1 = alloc_frame();
        const uintptr_t frame2 = alloc_frame();
        REQUIRE(frame0 != 0U);
        REQUIRE(frame1 == frame0 + Arch::PAGE_SIZE);
        REQUIRE(frame2 == frame1 + Arch::PAGE_SIZE);

        // Create a 1-page hole at frame1; requesting 3 contiguous frames must skip [frame0..frame2]
        free_frame(frame1);

        const uintptr_t span = alloc_frames(3U);
        REQUIRE(span != 0U);
        CHECK((span % Arch::PAGE_SIZE) == 0U);
        CHECK(span >= frame2 + Arch::PAGE_SIZE);
        for (size_t idx = 0U; idx < 3U; ++idx) {
            CHECK(pfa_is_page_allocated(span + (idx * Arch::PAGE_SIZE)));
        }

        free_frames(span, 3U);
        free_frame(frame0);
        free_frame(frame2);
        CHECK(pfa_free_pages_count() == baseline_free);
    }

    SECTION("Zero-count and exhaustion handling") {
        CHECK(alloc_frames(0U) == 0U);

        size_t drained = 0U;
        while (alloc_frame() != 0U) {
            ++drained;
        }
        CHECK(drained == baseline_free);
        CHECK(pfa_free_pages_count() == 0U);
        CHECK(alloc_frame() == 0U);
        CHECK(alloc_frames(2U) == 0U);
    }
}

TEST_CASE("PFA atomic reference counting and defensive guards", "[pfa][mm][cow]") {
    init_standard_test_pfa();
    const size_t baseline_free = pfa_free_pages_count();

    SECTION("CoW page_get / page_put lifecycle frees only when ref_count reaches zero") {
        const uintptr_t shared = alloc_frame();
        REQUIRE(shared != 0U);
        REQUIRE(page_ref_count(shared) == 1U);

        page_get(shared);
        page_get(shared);
        CHECK(page_ref_count(shared) == 3U);

        page_put(shared);
        CHECK(page_ref_count(shared) == 2U);
        CHECK(pfa_is_page_allocated(shared));

        page_put(shared);
        CHECK(page_ref_count(shared) == 1U);
        CHECK(pfa_is_page_allocated(shared));

        page_put(shared);
        CHECK(page_ref_count(shared) == 0U);
        CHECK_FALSE(pfa_is_page_allocated(shared));
        CHECK(pfa_free_pages_count() == baseline_free);

        // Duplicate page_put or free_frame on an already free page must not underflow
        page_put(shared);
        free_frame(shared);
        CHECK(page_ref_count(shared) == 0U);
        CHECK(pfa_free_pages_count() == baseline_free);
    }

    SECTION("Reserved frames and invalid addresses are safely ignored") {
        const uint32_t reserved_refs = page_ref_count(INITRD_START);
        page_get(INITRD_START);
        page_put(INITRD_START);
        free_frame(INITRD_START);
        CHECK(page_ref_count(INITRD_START) == reserved_refs);
        CHECK(pfa_is_page_allocated(INITRD_START));

        // Unaligned and out-of-bounds physical addresses
        free_frame(RAM_BASE + 123U);
        free_frame(0x1000U);
        free_frame(RAM_BASE + RAM_SIZE + Arch::PAGE_SIZE);
        CHECK(page_from_paddr(0x1000U) == nullptr);
        CHECK(page_to_paddr(nullptr) == 0U);
        CHECK(pfa_free_pages_count() == baseline_free);
    }
}