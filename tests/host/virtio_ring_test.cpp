#include "drivers/virtio/virtio_ring.hpp"
#include "memory_config.hpp"
#include "mm/pfa.hpp"
#include "mocks/host_hal.hpp"

#include <array>
#include <cstdint>
#include <snitch/snitch.hpp>

namespace {

using namespace Kernel::Drivers::Virtio;

void init_virtio_test_pfa() noexcept {
    HostTest::reset_host_hal();
    HAL::get_mmu().activate();

    Kernel::Boot::MemoryMap mem_map{};
    mem_map.regions[0] = Kernel::Boot::MemoryRegion{
        .base_paddr = HostTest::SIM_RAM_BASE,
        .size = HostTest::SIM_RAM_SIZE,
    };
    mem_map.count = 1U;
    mem_map.min_paddr = HostTest::SIM_RAM_BASE;
    mem_map.max_paddr = HostTest::SIM_RAM_BASE + HostTest::SIM_RAM_SIZE;
    mem_map.total_usable_bytes = HostTest::SIM_RAM_SIZE;

    Kernel::MM::pfa_init(mem_map, HostTest::SIM_RAM_BASE, HostTest::SIM_RAM_BASE + Arch::PAGE_SIZE,
                         0U, 0U, 0U, 0U, nullptr);
}

} // namespace

TEST_CASE("SplitVirtqueue descriptor allocation, chaining, and wrap-around", "[virtio][vring]") {
    init_virtio_test_pfa();
    const size_t baseline_free = Kernel::MM::pfa_free_pages_count();
    REQUIRE(baseline_free > 16U);

    SECTION("allocates and frees individual descriptors and reclaims PFA frames on destroy") {
        SplitVirtqueue virtq{};
        REQUIRE_FALSE(virtq.init(0U));
        REQUIRE_FALSE(virtq.init(3U)); // Non-power-of-two rejected
        REQUIRE(virtq.init(8U));

        CHECK(virtq.is_initialized());
        CHECK(virtq.queue_size() == 8U);
        CHECK(virtq.num_free() == 8U);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free - 1U);

        std::array<uint16_t, 8> allocated{};
        for (size_t idx = 0U; idx < 8U; ++idx) {
            allocated[idx] = virtq.alloc_desc();
            REQUIRE(allocated[idx] != VRING_INVALID_DESC);
        }
        CHECK(virtq.num_free() == 0U);
        CHECK(virtq.alloc_desc() == VRING_INVALID_DESC);

        // Double-free is rejected safely
        CHECK(virtq.free_desc(allocated[0]));
        CHECK_FALSE(virtq.free_desc(allocated[0]));
        CHECK(virtq.num_free() == 1U);

        for (size_t idx = 1U; idx < 8U; ++idx) {
            REQUIRE(virtq.free_desc(allocated[idx]));
        }
        CHECK(virtq.num_free() == 8U);

        virtq.destroy();
        CHECK_FALSE(virtq.is_initialized());
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free);
    }

    SECTION("submits 3-descriptor chains and drains simulated hypervisor Used Ring completions") {
        SplitVirtqueue virtq{};
        REQUIRE(virtq.init(8U));

        const VirtqBuffer buffers[3] = {
            VirtqBuffer{.paddr = 0x40100000ULL, .len = 16U, .device_writable = false},
            VirtqBuffer{.paddr = 0x40101000ULL, .len = 512U, .device_writable = true},
            VirtqBuffer{.paddr = 0x40102000ULL, .len = 1U, .device_writable = true},
        };

        uint16_t head_idx = VRING_INVALID_DESC;
        REQUIRE(virtq.submit_chain(buffers, 3U, &head_idx));
        REQUIRE(head_idx != VRING_INVALID_DESC);
        CHECK(virtq.num_free() == 5U);
        CHECK(virtq.avail_idx() == 1U);
        CHECK(virtq.avail_ring()[0] == head_idx);

        // Verify descriptor chain flags and next links
        const VringDesc* const descs = virtq.desc_table();
        const uint16_t second_idx = descs[head_idx].next;
        const uint16_t third_idx = descs[second_idx].next;

        CHECK(descs[head_idx].addr == 0x40100000ULL);
        CHECK(descs[head_idx].len == 16U);
        CHECK(descs[head_idx].flags == VRING_DESC_F_NEXT);

        CHECK(descs[second_idx].addr == 0x40101000ULL);
        CHECK(descs[second_idx].len == 512U);
        CHECK(descs[second_idx].flags == (VRING_DESC_F_NEXT | VRING_DESC_F_WRITE));

        CHECK(descs[third_idx].addr == 0x40102000ULL);
        CHECK(descs[third_idx].len == 1U);
        CHECK(descs[third_idx].flags == VRING_DESC_F_WRITE);

        CHECK_FALSE(virtq.has_used());

        // Simulate hypervisor completing the request in the Used Ring
        VringUsedHeader* const used_hdr = virtq.used_header();
        VringUsedElem* const used_ring = virtq.used_ring();
        used_ring[0] = VringUsedElem{.id = head_idx, .len = 513U};
        used_hdr->idx = 1U;

        REQUIRE(virtq.has_used());
        uint16_t completed_head = VRING_INVALID_DESC;
        uint32_t written_len = 0U;
        REQUIRE(virtq.pop_used(completed_head, written_len, true));
        CHECK(completed_head == head_idx);
        CHECK(written_len == 513U);
        CHECK(virtq.num_free() == 8U);
        CHECK_FALSE(virtq.has_used());

        // Exercise ring index wrap-around across 20 iterations (> queue_size of 8)
        for (uint16_t iter = 0U; iter < 20U; ++iter) {
            uint16_t req_head = VRING_INVALID_DESC;
            REQUIRE(virtq.submit_chain(buffers, 3U, &req_head));
            const uint16_t expected_avail = static_cast<uint16_t>(iter + 2U);
            CHECK(virtq.avail_idx() == expected_avail);

            const uint16_t used_slot = static_cast<uint16_t>((iter + 1U) % 8U);
            used_ring[used_slot] = VringUsedElem{.id = req_head, .len = 513U};
            used_hdr->idx = expected_avail;

            uint16_t popped_head = VRING_INVALID_DESC;
            uint32_t popped_len = 0U;
            REQUIRE(virtq.pop_used(popped_head, popped_len, true));
            CHECK(popped_head == req_head);
            CHECK(popped_len == 513U);
            CHECK(virtq.num_free() == 8U);
        }

        virtq.destroy();
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free);
    }
}