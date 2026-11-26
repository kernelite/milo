#pragma once

#include "fs/vfs.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::FS {

void devfs_reset() noexcept;

// Initializes devfs, mounts it at `/dev`, and registers `/dev/console`, `/dev/tty`, `/dev/null`.
[[nodiscard]] int32_t devfs_init_and_mount() noexcept;

// Registers a character device node inside `/dev`.
[[nodiscard]] int32_t devfs_register_chrdev(StringView name, uint32_t mode, uint64_t rdev,
                                            const FileOperations* f_ops) noexcept;

// Opens a File backed by `/dev/console` (used for initial stdio FDs 0, 1, 2).
[[nodiscard]] int32_t devfs_open_console(uint32_t flags, File** out_file) noexcept;

// Console TTY polling and foreground job-control helpers.
void console_poll_rx() noexcept;
void console_set_foreground_pgid(uint32_t pgid) noexcept;
[[nodiscard]] uint32_t console_get_foreground_pgid() noexcept;
void console_reset_tty() noexcept;

} // namespace Kernel::FS