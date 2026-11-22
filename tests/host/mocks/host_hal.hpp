#pragma once

#include "hal/mmu.hpp"

#include <cstddef>
#include <cstdint>

namespace HostTest {

constexpr uintptr_t SIM_RAM_BASE = 0x40000000ULL;
constexpr size_t SIM_RAM_SIZE = 16UL * 1024UL * 1024UL; // 16 MiB simulated physical RAM

struct MockStats {
    size_t console_bytes_written{0U};
    size_t dcache_flushes{0U};
    size_t icache_invalidations{0U};
    size_t active_address_spaces{0U};
    size_t mapped_page_count{0U};
};

// Resets all simulated RAM, page tables, console buffers, and failure injection flags.
void reset_host_hal() noexcept;

// Configures simulated failure injection for AddressSpaceBackend mocks.
void set_fail_create_root(bool should_fail) noexcept;
void set_fail_map_after(size_t successful_maps_before_failure) noexcept;

// Queries mock telemetry and simulated physical memory.
MockStats get_mock_stats() noexcept;
const char* get_captured_console_output() noexcept;
bool lookup_mapped_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t& out_paddr,
                        HAL::PageFlags& out_flags) noexcept;

} // namespace HostTest