#pragma once

#include "fs/Vfs.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::FS {

inline constexpr size_t kTmpfsMaxInodes = 256U;
inline constexpr size_t kTmpfsMaxPagesPerFile = 256U; // 1 MiB per file (256 * 4 KiB)

void tmpfs_reset() noexcept;

// Allocates and initializes an empty writable tmpfs Superblock.
[[nodiscard]] int32_t tmpfs_create_sb(Superblock** out_sb) noexcept;

// Creates a tmpfs instance and mounts it at `/tmp`.
[[nodiscard]] int32_t tmpfs_init_and_mount() noexcept;

} // namespace Kernel::FS