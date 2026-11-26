#pragma once

#include "fs/VfsTypes.hpp"

namespace Kernel::FS {

class Mount final {
  public:
    constexpr Mount() noexcept = default;
    ~Mount() noexcept = default;

    Mount(const Mount&) = delete;
    Mount& operator=(const Mount&) = delete;
    Mount(Mount&&) = delete;
    Mount& operator=(Mount&&) = delete;

    void reset() noexcept {
        m_mountpoint = nullptr;
        m_sb = nullptr;
        m_parent_mount = nullptr;
        m_active = false;
    }

    void activate(Dentry* mp, Superblock* superblock, Mount* parent) noexcept {
        m_mountpoint = mp;
        m_sb = superblock;
        m_parent_mount = parent;
        m_active = true;
    }

    [[nodiscard]] constexpr Dentry* get_mountpoint() const noexcept { return m_mountpoint; }
    constexpr void set_mountpoint(Dentry* mp) noexcept { m_mountpoint = mp; }

    [[nodiscard]] constexpr Superblock* get_sb() const noexcept { return m_sb; }
    constexpr void set_sb(Superblock* superblock) noexcept { m_sb = superblock; }

    [[nodiscard]] constexpr Mount* get_parent_mount() const noexcept { return m_parent_mount; }
    constexpr void set_parent_mount(Mount* parent) noexcept { m_parent_mount = parent; }

    [[nodiscard]] constexpr bool is_active() const noexcept { return m_active; }

  private:
    Dentry* m_mountpoint{nullptr};
    Superblock* m_sb{nullptr};
    Mount* m_parent_mount{nullptr};
    bool m_active{false};
};

} // namespace Kernel::FS
