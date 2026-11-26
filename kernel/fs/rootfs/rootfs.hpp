#pragma once

#include "boot/fdt.hpp"
#include "fs/vfs.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::FS {

void rootfs_reset() noexcept;

// Populates a read-only VFS Superblock/Dentry/Inode hierarchy from a CPIO `newc` archive.
[[nodiscard]] int32_t rootfs_create_from_cpio(const void* cpio_base, size_t cpio_size,
                                              Superblock** out_sb) noexcept;

// Builds the read-only CPIO rootfs and mounts it at `/`.
[[nodiscard]] int32_t rootfs_mount_cpio(const void* cpio_base, size_t cpio_size) noexcept;

// Convenience boot helper for mounting the DTB-discovered initrd at `/`.
[[nodiscard]] int32_t rootfs_mount_from_initrd(const Kernel::Boot::InitrdLocation& initrd) noexcept;

} // namespace Kernel::FS
