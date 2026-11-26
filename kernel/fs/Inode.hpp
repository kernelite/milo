#pragma once

#include "fs/VfsTypes.hpp"

namespace Kernel::FS {

struct InodeOperations {
    int32_t (*lookup)(Inode* dir, Dentry* target){nullptr};
    int32_t (*create)(Inode* dir, Dentry* target, uint32_t mode){nullptr};
    int32_t (*mkdir)(Inode* dir, Dentry* target, uint32_t mode){nullptr};
    int32_t (*unlink)(Inode* dir, Dentry* target){nullptr};
    int32_t (*rmdir)(Inode* dir, Dentry* target){nullptr};
    int32_t (*rename)(Inode* old_dir, Dentry* old_dentry, Inode* new_dir,
                      Dentry* new_dentry){nullptr};
    int32_t (*truncate)(Inode* inode, uint64_t new_size){nullptr};
    int32_t (*readlink)(Inode* inode, char* buf, size_t buf_size, size_t* out_len){nullptr};
};

struct Inode {
    uint64_t ino{0U};
    uint32_t mode{0U};
    uint32_t uid{0U};
    uint32_t gid{0U};
    uint64_t size{0U};
    uint64_t rdev{0U};
    uint32_t nlink{1U};
    uint64_t atime_ns{0U};
    uint64_t mtime_ns{0U};
    uint64_t ctime_ns{0U};
    uint32_t refcount{0U};
    Superblock* sb{nullptr};
    const InodeOperations* i_ops{nullptr};
    const FileOperations* f_ops{nullptr};
    void* fs_private{nullptr};
    bool active{false};

    [[nodiscard]] bool is_dir() const noexcept { return (mode & kModeTypeMask) == kModeDir; }
    [[nodiscard]] bool is_reg() const noexcept { return (mode & kModeTypeMask) == kModeReg; }
    [[nodiscard]] bool is_chr() const noexcept { return (mode & kModeTypeMask) == kModeChr; }
    [[nodiscard]] bool is_fifo() const noexcept { return (mode & kModeTypeMask) == kModeFifo; }
    [[nodiscard]] bool is_symlink() const noexcept { return (mode & kModeTypeMask) == kModeLnk; }
};

} // namespace Kernel::FS
