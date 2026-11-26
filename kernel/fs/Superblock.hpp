#pragma once

#include "fs/VfsTypes.hpp"

namespace Kernel::FS {

struct SuperblockOperations {
    int32_t (*sync_fs)(Superblock* sb){nullptr};
    Inode* (*alloc_inode)(Superblock* sb){nullptr};
    void (*destroy_inode)(Inode* inode){nullptr};
};

class Superblock final {
  public:
    constexpr Superblock() noexcept = default;
    ~Superblock() noexcept = default;

    Superblock(const Superblock&) = delete;
    Superblock& operator=(const Superblock&) = delete;
    Superblock(Superblock&&) = delete;
    Superblock& operator=(Superblock&&) = delete;

    void reset() noexcept {
        m_fs_type = nullptr;
        m_block_size = 4096U;
        m_flags = 0U;
        m_root = nullptr;
        m_ops = nullptr;
        m_fs_private = nullptr;
        m_active = false;
    }

    void activate() noexcept {
        reset();
        m_active = true;
    }

    [[nodiscard]] constexpr const char* get_fs_type() const noexcept { return m_fs_type; }
    constexpr void set_fs_type(const char* new_type) noexcept { m_fs_type = new_type; }

    [[nodiscard]] constexpr uint32_t get_block_size() const noexcept { return m_block_size; }
    constexpr void set_block_size(uint32_t new_block_size) noexcept {
        m_block_size = new_block_size;
    }

    [[nodiscard]] constexpr uint64_t get_flags() const noexcept { return m_flags; }
    constexpr void set_flags(uint64_t new_flags) noexcept { m_flags = new_flags; }

    [[nodiscard]] constexpr Dentry* get_root() const noexcept { return m_root; }
    constexpr void set_root(Dentry* new_root) noexcept { m_root = new_root; }

    [[nodiscard]] constexpr const SuperblockOperations* get_ops() const noexcept { return m_ops; }
    constexpr void set_ops(const SuperblockOperations* new_ops) noexcept { m_ops = new_ops; }

    [[nodiscard]] constexpr void* get_fs_private() const noexcept { return m_fs_private; }
    constexpr void set_fs_private(void* new_private) noexcept { m_fs_private = new_private; }

    [[nodiscard]] constexpr bool is_active() const noexcept { return m_active; }
    constexpr void set_active(bool active) noexcept { m_active = active; }

    [[nodiscard]] constexpr bool is_read_only() const noexcept {
        return (m_flags & kSbReadOnly) != 0U;
    }

    [[nodiscard]] int32_t sync() noexcept {
        if (m_ops != nullptr && m_ops->sync_fs != nullptr) {
            return m_ops->sync_fs(this);
        }
        return kOk;
    }

    [[nodiscard]] Inode* alloc_inode() noexcept {
        if (m_ops != nullptr && m_ops->alloc_inode != nullptr) {
            return m_ops->alloc_inode(this);
        }
        return nullptr;
    }

    [[nodiscard]] bool destroy_inode(Inode* inode) const noexcept {
        if (m_ops != nullptr && m_ops->destroy_inode != nullptr) {
            m_ops->destroy_inode(inode);
            return true;
        }
        return false;
    }

  private:
    const char* m_fs_type{nullptr};
    uint32_t m_block_size{4096U};
    uint64_t m_flags{0U};
    Dentry* m_root{nullptr};
    const SuperblockOperations* m_ops{nullptr};
    void* m_fs_private{nullptr};
    bool m_active{false};
};

} // namespace Kernel::FS
