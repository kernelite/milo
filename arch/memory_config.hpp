#pragma once

#include <cstddef>
#include <cstdint>

namespace Arch {

// QEMU virt AArch64 Physical Memory Parameters
constexpr uintptr_t RAM_BASE = 0x40000000U;
constexpr size_t PAGE_SIZE = 4096U; // 0x1000
constexpr size_t PAGE_SHIFT = 12U;
constexpr size_t MAX_RAM_SIZE = 512UL * 1024UL * 1024UL; // 512 MB max platform RAM

} // namespace Arch