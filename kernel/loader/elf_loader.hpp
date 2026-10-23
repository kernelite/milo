#pragma once

#include "core/traits.hpp"
#include "loader/elf64.hpp"
#include "mm/address_space.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Loader {

// Auxiliary Vector Type Definition
struct Elf64_auxv_t {
    uint64_t a_type;
    union {
        uint64_t a_val;
    } a_un;
};

enum class ElfLoaderError {
    Success = 0,
    InvalidElf,
    NoLoadSegments,
    OutOfMemory,
    PageMappingFailed,
    InvalidSegmentAlignment,
    ArithmeticOverflow,
    SegmentOutOfBounds,
};

struct LoadedElf {
    Kernel::MM::AddressSpace address_space;
    uintptr_t entry_point{0U};
    size_t loaded_segment_count{0U};
};

// Function prototypes
Result<LoadedElf, ElfLoaderError> load_elf(const uint8_t* elf_data, size_t size) noexcept;

inline Result<LoadedElf, ElfLoaderError> load_elf(const void* elf_data, size_t size) noexcept {
    return load_elf(static_cast<const uint8_t*>(elf_data), size);
}

} // namespace Kernel::Loader