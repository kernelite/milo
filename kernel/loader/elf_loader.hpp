#pragma once

#include "loader/elf64.hpp"
#include "mm/address_space.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Loader {

enum class ElfLoaderError {
    Success = 0,
    InvalidElf,
    NoLoadSegments,
    OutOfMemory,
    PageMappingFailed,
    InvalidSegmentAlignment,
    ArithmeticOverflow
};

const char* elf_loader_error_str(ElfLoaderError error) noexcept;

constexpr HAL::PageFlags translate_segment_flags(uint32_t p_flags) noexcept {
    HAL::PageFlags flags = HAL::PageFlags::User;
    if ((p_flags & PF_R) != 0U) {
        flags = flags | HAL::PageFlags::Read;
    }
    if ((p_flags & PF_W) != 0U) {
        flags = flags | HAL::PageFlags::Write;
    }
    if ((p_flags & PF_X) != 0U) {
        flags = flags | HAL::PageFlags::Execute;
    }
    return flags;
}

struct LoadedElf {
    MM::AddressSpace address_space;
    uintptr_t entry_point{0U};
    size_t loaded_segment_count{0U};
};

Result<LoadedElf, ElfLoaderError> load_elf(const uint8_t* elf_data, size_t elf_size) noexcept;

inline Result<LoadedElf, ElfLoaderError> load_elf(const void* elf_data, size_t elf_size) noexcept {
    return load_elf(static_cast<const uint8_t*>(elf_data), elf_size);
}

} // namespace Kernel::Loader