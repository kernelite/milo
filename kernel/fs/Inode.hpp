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

    void retain() noexcept;
    void release() noexcept;

    [[nodiscard]] constexpr uint64_t get_ino() const noexcept { return ino; }
    constexpr void set_ino(uint64_t new_ino) noexcept { ino = new_ino; }

    [[nodiscard]] constexpr uint32_t get_mode() const noexcept { return mode; }
    constexpr void set_mode(uint32_t new_mode) noexcept { mode = new_mode; }

    [[nodiscard]] constexpr uint32_t get_uid() const noexcept { return uid; }
    constexpr void set_uid(uint32_t new_uid) noexcept { uid = new_uid; }

    [[nodiscard]] constexpr uint32_t get_gid() const noexcept { return gid; }
    constexpr void set_gid(uint32_t new_gid) noexcept { gid = new_gid; }

    [[nodiscard]] constexpr uint64_t get_size() const noexcept { return size; }
    constexpr void set_size(uint64_t new_size) noexcept { size = new_size; }

    [[nodiscard]] constexpr uint64_t get_rdev() const noexcept { return rdev; }
    constexpr void set_rdev(uint64_t new_rdev) noexcept { rdev = new_rdev; }

    [[nodiscard]] constexpr uint32_t get_nlink() const noexcept { return nlink; }
    constexpr void set_nlink(uint32_t new_nlink) noexcept { nlink = new_nlink; }

    [[nodiscard]] constexpr uint64_t get_atime_ns() const noexcept { return atime_ns; }
    constexpr void set_atime_ns(uint64_t new_atime) noexcept { atime_ns = new_atime; }

    [[nodiscard]] constexpr uint64_t get_mtime_ns() const noexcept { return mtime_ns; }
    constexpr void set_mtime_ns(uint64_t new_mtime) noexcept { mtime_ns = new_mtime; }

    [[nodiscard]] constexpr uint64_t get_ctime_ns() const noexcept { return ctime_ns; }
    constexpr void set_ctime_ns(uint64_t new_ctime) noexcept { ctime_ns = new_ctime; }

    [[nodiscard]] constexpr uint32_t get_refcount() const noexcept { return refcount; }
    [[nodiscard]] constexpr bool is_active() const noexcept { return active; }

    [[nodiscard]] constexpr Superblock* get_sb() const noexcept { return sb; }
    constexpr void set_sb(Superblock* new_sb) noexcept { sb = new_sb; }

    [[nodiscard]] constexpr const InodeOperations* get_i_ops() const noexcept { return i_ops; }
    constexpr void set_i_ops(const InodeOperations* new_ops) noexcept { i_ops = new_ops; }

    [[nodiscard]] constexpr const FileOperations* get_f_ops() const noexcept { return f_ops; }
    constexpr void set_f_ops(const FileOperations* new_ops) noexcept { f_ops = new_ops; }

    [[nodiscard]] constexpr void* get_fs_private() const noexcept { return fs_private; }
    constexpr void set_fs_private(void* new_private) noexcept { fs_private = new_private; }

    [[nodiscard]] constexpr bool is_dir() const noexcept {
        return (mode & kModeTypeMask) == kModeDir;
    }
    [[nodiscard]] constexpr bool is_reg() const noexcept {
        return (mode & kModeTypeMask) == kModeReg;
    }
    [[nodiscard]] constexpr bool is_chr() const noexcept {
        return (mode & kModeTypeMask) == kModeChr;
    }
    [[nodiscard]] constexpr bool is_fifo() const noexcept {
        return (mode & kModeTypeMask) == kModeFifo;
    }
    [[nodiscard]] constexpr bool is_symlink() const noexcept {
        return (mode & kModeTypeMask) == kModeLnk;
    }

    [[nodiscard]] int32_t lookup(Dentry* target) noexcept {
        if (i_ops == nullptr || i_ops->lookup == nullptr) {
            return kEnoent;
        }
        return i_ops->lookup(this, target);
    }

    [[nodiscard]] int32_t create(Dentry* target, uint32_t new_mode) noexcept {
        if (i_ops == nullptr || i_ops->create == nullptr) {
            return kEopnotsupp;
        }
        return i_ops->create(this, target, new_mode);
    }

    [[nodiscard]] int32_t mkdir(Dentry* target, uint32_t new_mode) noexcept {
        if (i_ops == nullptr || i_ops->mkdir == nullptr) {
            return kEopnotsupp;
        }
        return i_ops->mkdir(this, target, new_mode);
    }

    [[nodiscard]] int32_t unlink(Dentry* target) noexcept {
        if (i_ops == nullptr || i_ops->unlink == nullptr) {
            return kEopnotsupp;
        }
        return i_ops->unlink(this, target);
    }

    [[nodiscard]] int32_t rmdir(Dentry* target) noexcept {
        if (i_ops == nullptr || i_ops->rmdir == nullptr) {
            return kEopnotsupp;
        }
        return i_ops->rmdir(this, target);
    }

    [[nodiscard]] int32_t rename(Dentry* old_dentry, Inode* new_dir, Dentry* new_dentry) noexcept {
        if (i_ops == nullptr || i_ops->rename == nullptr) {
            return kEopnotsupp;
        }
        return i_ops->rename(this, old_dentry, new_dir, new_dentry);
    }

    [[nodiscard]] int32_t truncate(uint64_t new_size) noexcept {
        if (i_ops != nullptr && i_ops->truncate != nullptr) {
            return i_ops->truncate(this, new_size);
        }
        size = new_size;
        return kOk;
    }

    [[nodiscard]] int32_t readlink(char* buf, size_t buf_size, size_t* out_len) noexcept {
        if (i_ops == nullptr || i_ops->readlink == nullptr) {
            return kEopnotsupp;
        }
        return i_ops->readlink(this, buf, buf_size, out_len);
    }
};

} // namespace Kernel::FS
