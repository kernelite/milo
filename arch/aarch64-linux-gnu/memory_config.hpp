#pragma once

#include <cstddef>
#include <cstdint>

namespace Arch::AArch64 {

// QEMU virt AArch64 Physical Memory Parameters
constexpr uintptr_t RAM_BASE = 0x40000000U;
constexpr size_t PAGE_SIZE = 4096U; // 0x1000
constexpr size_t PAGE_SHIFT = 12U;
constexpr size_t BLOCK_SIZE_2M = 2UL * 1024UL * 1024UL; // 0x200000
constexpr size_t BLOCK_SHIFT_2M = 21U;
constexpr size_t MAX_RAM_SIZE = 64ULL * 1024ULL * 1024ULL * 1024ULL; // 64 GiB PA bound

} // namespace Arch::AArch64