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

class Inode final {
  public:
    constexpr Inode() noexcept = default;
    ~Inode() noexcept = default;

    Inode(const Inode&) = delete;
    Inode& operator=(const Inode&) = delete;
    Inode(Inode&&) = delete;
    Inode& operator=(Inode&&) = delete;

    void reset() noexcept {
        m_ino = 0U;
        m_mode = 0U;
        m_uid = 0U;
        m_gid = 0U;
        m_size = 0U;
        m_rdev = 0U;
        m_nlink = 1U;
        m_atime_ns = 0U;
        m_mtime_ns = 0U;
        m_ctime_ns = 0U;
        m_refcount = 0U;
        m_sb = nullptr;
        m_i_ops = nullptr;
        m_f_ops = nullptr;
        m_fs_private = nullptr;
        m_active = false;
    }

    void activate(Superblock* sb_ptr) noexcept {
        reset();
        m_sb = sb_ptr;
        m_refcount = 1U;
        m_active = true;
    }

    void retain() noexcept;
    void release() noexcept;

    [[nodiscard]] constexpr uint64_t get_ino() const noexcept { return m_ino; }
    constexpr void set_ino(uint64_t new_ino) noexcept { m_ino = new_ino; }

    [[nodiscard]] constexpr uint32_t get_mode() const noexcept { return m_mode; }
    constexpr void set_mode(uint32_t new_mode) noexcept { m_mode = new_mode; }

    [[nodiscard]] constexpr uint32_t get_uid() const noexcept { return m_uid; }
    constexpr void set_uid(uint32_t new_uid) noexcept { m_uid = new_uid; }

    [[nodiscard]] constexpr uint32_t get_gid() const noexcept { return m_gid; }
    constexpr void set_gid(uint32_t new_gid) noexcept { m_gid = new_gid; }

    [[nodiscard]] constexpr uint64_t get_size() const noexcept { return m_size; }
    constexpr void set_size(uint64_t new_size) noexcept { m_size = new_size; }

    [[nodiscard]] constexpr uint64_t get_rdev() const noexcept { return m_rdev; }
    constexpr void set_rdev(uint64_t new_rdev) noexcept { m_rdev = new_rdev; }

    [[nodiscard]] constexpr uint32_t get_nlink() const noexcept { return m_nlink; }
    constexpr void set_nlink(uint32_t new_nlink) noexcept { m_nlink = new_nlink; }

    [[nodiscard]] constexpr uint64_t get_atime_ns() const noexcept { return m_atime_ns; }
    constexpr void set_atime_ns(uint64_t new_atime) noexcept { m_atime_ns = new_atime; }

    [[nodiscard]] constexpr uint64_t get_mtime_ns() const noexcept { return m_mtime_ns; }
    constexpr void set_mtime_ns(uint64_t new_mtime) noexcept { m_mtime_ns = new_mtime; }

    [[nodiscard]] constexpr uint64_t get_ctime_ns() const noexcept { return m_ctime_ns; }
    constexpr void set_ctime_ns(uint64_t new_ctime) noexcept { m_ctime_ns = new_ctime; }

    [[nodiscard]] constexpr uint32_t get_refcount() const noexcept { return m_refcount; }
    constexpr void set_refcount(uint32_t new_refcount) noexcept { m_refcount = new_refcount; }

    [[nodiscard]] constexpr bool is_active() const noexcept { return m_active; }
    constexpr void set_active(bool active) noexcept { m_active = active; }

    [[nodiscard]] constexpr Superblock* get_sb() const noexcept { return m_sb; }
    constexpr void set_sb(Superblock* new_sb) noexcept { m_sb = new_sb; }

    [[nodiscard]] constexpr const InodeOperations* get_i_ops() const noexcept { return m_i_ops; }
    constexpr void set_i_ops(const InodeOperations* new_ops) noexcept { m_i_ops = new_ops; }

    [[nodiscard]] constexpr const FileOperations* get_f_ops() const noexcept { return m_f_ops; }
    constexpr void set_f_ops(const FileOperations* new_ops) noexcept { m_f_ops = new_ops; }

    [[nodiscard]] constexpr void* get_fs_private() const noexcept { return m_fs_private; }
    constexpr void set_fs_private(void* new_private) noexcept { m_fs_private = new_private; }

    [[nodiscard]] constexpr bool is_dir() const noexcept {
        return (m_mode & kModeTypeMask) == kModeDir;
    }
    [[nodiscard]] constexpr bool is_reg() const noexcept {
        return (m_mode & kModeTypeMask) == kModeReg;
    }
    [[nodiscard]] constexpr bool is_chr() const noexcept {
        return (m_mode & kModeTypeMask) == kModeChr;
    }
    [[nodiscard]] constexpr bool is_blk() const noexcept {
        return (m_mode & kModeTypeMask) == kModeBlk;
    }
    [[nodiscard]] constexpr bool is_fifo() const noexcept {
        return (m_mode & kModeTypeMask) == kModeFifo;
    }
    [[nodiscard]] constexpr bool is_symlink() const noexcept {
        return (m_mode & kModeTypeMask) == kModeLnk;
    }

    [[nodiscard]] int32_t lookup(Dentry* target) noexcept {
        if (m_i_ops == nullptr || m_i_ops->lookup == nullptr) {
            return kEnoent;
        }
        return m_i_ops->lookup(this, target);
    }

    [[nodiscard]] int32_t create(Dentry* target, uint32_t new_mode) noexcept {
        if (m_i_ops == nullptr || m_i_ops->create == nullptr) {
            return kEopnotsupp;
        }
        return m_i_ops->create(this, target, new_mode);
    }

    [[nodiscard]] int32_t mkdir(Dentry* target, uint32_t new_mode) noexcept {
        if (m_i_ops == nullptr || m_i_ops->mkdir == nullptr) {
            return kEopnotsupp;
        }
        return m_i_ops->mkdir(this, target, new_mode);
    }

    [[nodiscard]] int32_t unlink(Dentry* target) noexcept {
        if (m_i_ops == nullptr || m_i_ops->unlink == nullptr) {
            return kEopnotsupp;
        }
        return m_i_ops->unlink(this, target);
    }

    [[nodiscard]] int32_t rmdir(Dentry* target) noexcept {
        if (m_i_ops == nullptr || m_i_ops->rmdir == nullptr) {
            return kEopnotsupp;
        }
        return m_i_ops->rmdir(this, target);
    }

    [[nodiscard]] int32_t rename(Dentry* old_dentry, Inode* new_dir, Dentry* new_dentry) noexcept {
        if (m_i_ops == nullptr || m_i_ops->rename == nullptr) {
            return kEopnotsupp;
        }
        return m_i_ops->rename(this, old_dentry, new_dir, new_dentry);
    }

    [[nodiscard]] int32_t truncate(uint64_t new_size) noexcept {
        if (m_i_ops != nullptr && m_i_ops->truncate != nullptr) {
            return m_i_ops->truncate(this, new_size);
        }
        m_size = new_size;
        return kOk;
    }

    [[nodiscard]] int32_t readlink(char* buf, size_t buf_size, size_t* out_len) noexcept {
        if (m_i_ops == nullptr || m_i_ops->readlink == nullptr) {
            return kEopnotsupp;
        }
        return m_i_ops->readlink(this, buf, buf_size, out_len);
    }

  private:
    uint64_t m_ino{0U};
    uint32_t m_mode{0U};
    uint32_t m_uid{0U};
    uint32_t m_gid{0U};
    uint64_t m_size{0U};
    uint64_t m_rdev{0U};
    uint32_t m_nlink{1U};
    uint64_t m_atime_ns{0U};
    uint64_t m_mtime_ns{0U};
    uint64_t m_ctime_ns{0U};
    uint32_t m_refcount{0U};
    Superblock* m_sb{nullptr};
    const InodeOperations* m_i_ops{nullptr};
    const FileOperations* m_f_ops{nullptr};
    void* m_fs_private{nullptr};
    bool m_active{false};
};

} // namespace Kernel::FS
