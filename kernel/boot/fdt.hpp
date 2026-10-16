#pragma once

#include <cstddef>
#include <cstdint>

namespace FDT {

constexpr uint32_t FDT_MAGIC = 0xD00DFEED;

// Standard FDT Header layout (all multi-byte fields are Big-Endian in memory)
struct Header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

struct InitrdLocation {
    uintptr_t start_paddr{0};
    uintptr_t end_paddr{0};
    size_t size{0};
    bool valid{false};
};

// Check if memory at `fdt_paddr` contains a valid Flattened Device Tree
bool is_valid(uintptr_t fdt_paddr) noexcept;

// Read total byte size of the DTB from its header
uint32_t get_total_size(uintptr_t fdt_paddr) noexcept;

// In-place scan of `/chosen` node for `linux,initrd-start` and `linux,initrd-end`
InitrdLocation find_initrd(uintptr_t fdt_paddr) noexcept;

} // namespace FDT