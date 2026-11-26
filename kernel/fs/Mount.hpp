#pragma once

#include "fs/VfsTypes.hpp"

namespace Kernel::FS {

struct Mount {
    Dentry* mountpoint{nullptr};
    Superblock* sb{nullptr};
    Mount* parent_mount{nullptr};
    bool active{false};

    [[nodiscard]] constexpr Dentry* get_mountpoint() const noexcept { return mountpoint; }
    [[nodiscard]] constexpr Superblock* get_sb() const noexcept { return sb; }
    [[nodiscard]] constexpr Mount* get_parent_mount() const noexcept { return parent_mount; }
    [[nodiscard]] constexpr bool is_active() const noexcept { return active; }

    constexpr void activate(Dentry* new_mountpoint, Superblock* new_sb,
                            Mount* new_parent_mount) noexcept {
        mountpoint = new_mountpoint;
        sb = new_sb;
        parent_mount = new_parent_mount;
        active = true;
    }

    constexpr void reset() noexcept {
        mountpoint = nullptr;
        sb = nullptr;
        parent_mount = nullptr;
        active = false;
    }
};

} // namespace Kernel::FS
