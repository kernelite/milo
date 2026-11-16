#include "mm/pfa.hpp"
#include "mocks/host_hal.hpp"
#include "test_framework.hpp"

#include <vector>

TEST_CASE("PFA", "Bitmask Initialization, Single/Contiguous Allocations & Recycling") {
    HostHAL::reset();
    pfa_init(HostHAL::SIMULATED_RAM_BASE, HostHAL::SIMULATED_RAM_SIZE);

    const size_t total_pages = HostHAL::SIMULATED_RAM_SIZE / HostHAL::SIMULATED_PAGE_SIZE;
    EXPECT_EQ(pfa_allocated_pages_count(), 0U);
    EXPECT_EQ(pfa_free_pages_count(), total_pages);

    // Reserve a region and verify it is marked allocated and skipped
    const uintptr_t res_start = HostHAL::SIMULATED_RAM_BASE + (4U * HostHAL::SIMULATED_PAGE_SIZE);
    const uintptr_t res_end = HostHAL::SIMULATED_RAM_BASE + (8U * HostHAL::SIMULATED_PAGE_SIZE);
    pfa_reserve_range(res_start, res_end);
    EXPECT_EQ(pfa_allocated_pages_count(), 4U);
    EXPECT_TRUE(pfa_is_page_allocated(res_start));
    EXPECT_FALSE(pfa_is_page_allocated(HostHAL::SIMULATED_RAM_BASE));

    // Single frame allocation
    const uintptr_t frame0 = alloc_frame();
    EXPECT_EQ(frame0, HostHAL::SIMULATED_RAM_BASE);
    EXPECT_EQ(frame0 % HostHAL::SIMULATED_PAGE_SIZE, 0U);
    EXPECT_TRUE(pfa_is_page_allocated(frame0));

    // Contiguous multi-frame allocation skips reserved block [4..8)
    const uintptr_t block = alloc_frames(6U);
    EXPECT_EQ(block, res_end);
    for (size_t i = 0U; i < 6U; ++i) {
        EXPECT_TRUE(pfa_is_page_allocated(block + (i * HostHAL::SIMULATED_PAGE_SIZE)));
    }

    // Zero-count allocation returns 0
    EXPECT_EQ(alloc_frames(0U), 0U);

    // Free and verify immediate recycling
    free_frame(frame0);
    EXPECT_FALSE(pfa_is_page_allocated(frame0));
    const uintptr_t recycled = alloc_frame();
    EXPECT_EQ(recycled, frame0);

    free_frame(recycled);
    free_frames(block, 6U);
    EXPECT_EQ(pfa_allocated_pages_count(), 4U);
}

TEST_CASE("PFA", "Out-of-Bounds, Unaligned & Double-Free Safety Guards") {
    HostHAL::reset();
    pfa_init(HostHAL::SIMULATED_RAM_BASE, HostHAL::SIMULATED_RAM_SIZE);

    const uintptr_t frame = alloc_frame();
    EXPECT_NE(frame, 0U);
    const size_t alloc_before = pfa_allocated_pages_count();

    // 1. Unaligned address guard
    HostHAL::clear_console_output();
    free_frame(frame + 128U);
    EXPECT_EQ(pfa_allocated_pages_count(), alloc_before);
    EXPECT_NE(HostHAL::console_output().find("Unaligned physical frame address"),
              std::string::npos);

    // 2. Out-of-bounds address guards (below RAM_BASE and >= RAM_END)
    HostHAL::clear_console_output();
    free_frame(0x1000U);
    EXPECT_EQ(pfa_allocated_pages_count(), alloc_before);
    EXPECT_NE(HostHAL::console_output().find("Out of bounds address"), std::string::npos);

    HostHAL::clear_console_output();
    free_frame(HostHAL::SIMULATED_RAM_BASE + HostHAL::SIMULATED_RAM_SIZE);
    EXPECT_EQ(pfa_allocated_pages_count(), alloc_before);
    EXPECT_NE(HostHAL::console_output().find("Out of bounds address"), std::string::npos);

    // 3. Double-free guard
    free_frame(frame);
    EXPECT_EQ(pfa_allocated_pages_count(), alloc_before - 1U);

    HostHAL::clear_console_output();
    free_frame(frame);
    EXPECT_EQ(pfa_allocated_pages_count(), alloc_before - 1U);
    EXPECT_NE(HostHAL::console_output().find("Double-free detected"), std::string::npos);
}