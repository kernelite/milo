#pragma once

#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "mm/address_space.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace HostHAL {

constexpr uintptr_t SIMULATED_RAM_BASE = 0x40000000ULL;
constexpr size_t SIMULATED_RAM_SIZE = 256UL * 1024UL * 1024UL; // 256 MiB
constexpr size_t SIMULATED_MAX_RAM_SIZE = 512UL * 1024UL * 1024UL;
constexpr size_t SIMULATED_PAGE_SIZE = 4096U;

void reset() noexcept;

void* phys_to_host_ptr(uintptr_t paddr) noexcept;
uintptr_t host_ptr_to_phys(const void* ptr) noexcept;

const std::string& console_output() noexcept;
void clear_console_output() noexcept;

size_t dcache_flush_count() noexcept;
size_t icache_invalidate_count() noexcept;
size_t active_address_space_count() noexcept;

} // namespace HostHAL