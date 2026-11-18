#pragma once

#include <cstddef>
#include <cstdint>

namespace FDT {

struct MemoryRegion {
    uintptr_t base_paddr{0U};
    size_t size{0U};
};

struct MemoryMap {
    static constexpr size_t MAX_REGIONS = 16U;
    MemoryRegion regions[MAX_REGIONS]{};
    size_t count{0U};
    uintptr_t min_paddr{0U};
    uintptr_t max_paddr{0U};
    size_t total_usable_bytes{0U};
};

struct ReserveMap {
    static constexpr size_t MAX_ENTRIES = 32U;
    MemoryRegion entries[MAX_ENTRIES]{};
    size_t count{0U};
};

struct InitrdLocation {
    uintptr_t start_paddr{0U};
    uintptr_t end_paddr{0U};
    size_t size{0U};
    bool valid{false};
};

bool validate_header(uintptr_t dtb_paddr, size_t* out_totalsize = nullptr) noexcept;
bool find_memory_regions(uintptr_t dtb_paddr, MemoryMap& out_map) noexcept;
MemoryMap find_memory_regions(uintptr_t dtb_paddr) noexcept;
ReserveMap find_memory_reservations(uintptr_t dtb_paddr) noexcept;
InitrdLocation find_initrd(uintptr_t dtb_paddr) noexcept;

} // namespace FDT