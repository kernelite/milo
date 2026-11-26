#pragma once

#include "fs/VfsTypes.hpp"

namespace Kernel::FS {

struct SuperblockOperations {
    int32_t (*sync_fs)(Superblock* sb){nullptr};
    Inode* (*alloc_inode)(Superblock* sb){nullptr};
    void (*destroy_inode)(Inode* inode){nullptr};
};

struct Superblock {
    const char* fs_type{nullptr};
    uint32_t block_size{4096U};
    uint64_t flags{0U};
    Dentry* root{nullptr};
    const SuperblockOperations* ops{nullptr};
    void* fs_private{nullptr};
    bool active{false};

    [[nodiscard]] constexpr const char* get_fs_type() const noexcept { return fs_type; }
    constexpr void set_fs_type(const char* new_type) noexcept { fs_type = new_type; }

    [[nodiscard]] constexpr uint32_t get_block_size() const noexcept { return block_size; }
    constexpr void set_block_size(uint32_t new_block_size) noexcept { block_size = new_block_size; }

    [[nodiscard]] constexpr uint64_t get_flags() const noexcept { return flags; }
    constexpr void set_flags(uint64_t new_flags) noexcept { flags = new_flags; }

    [[nodiscard]] constexpr Dentry* get_root() const noexcept { return root; }
    constexpr void set_root(Dentry* new_root) noexcept { root = new_root; }

    [[nodiscard]] constexpr const SuperblockOperations* get_ops() const noexcept { return ops; }
    constexpr void set_ops(const SuperblockOperations* new_ops) noexcept { ops = new_ops; }

    [[nodiscard]] constexpr void* get_fs_private() const noexcept { return fs_private; }
    constexpr void set_fs_private(void* new_private) noexcept { fs_private = new_private; }

    [[nodiscard]] constexpr bool is_active() const noexcept { return active; }
    [[nodiscard]] constexpr bool is_read_only() const noexcept {
        return (flags & kSbReadOnly) != 0U;
    }

    [[nodiscard]] int32_t sync() noexcept {
        if (ops != nullptr && ops->sync_fs != nullptr) {
            return ops->sync_fs(this);
        }
        return kOk;
    }

    [[nodiscard]] Inode* alloc_inode() noexcept {
        if (ops != nullptr && ops->alloc_inode != nullptr) {
            return ops->alloc_inode(this);
        }
        return nullptr;
    }

    [[nodiscard]] bool destroy_inode(Inode* inode) const noexcept {
        if (ops != nullptr && ops->destroy_inode != nullptr) {
            ops->destroy_inode(inode);
            return true;
        }
        return false;
    }
};

} // namespace Kernel::FS
