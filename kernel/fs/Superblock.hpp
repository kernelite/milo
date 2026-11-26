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

    [[nodiscard]] bool is_read_only() const noexcept { return (flags & kSbReadOnly) != 0U; }
};

} // namespace Kernel::FS
