#pragma once

#include "fs/Dentry.hpp"
#include "fs/File.hpp"
#include "fs/Inode.hpp"
#include "fs/Mount.hpp"
#include "fs/Superblock.hpp"

namespace Kernel::FS {

struct VfsStats {
    size_t active_superblocks{0U};
    size_t active_mounts{0U};
    size_t active_inodes{0U};
    size_t active_dentries{0U};
    size_t active_files{0U};
};

// Lifecycle & pool management.
void vfs_reset() noexcept;
[[nodiscard]] VfsStats vfs_stats() noexcept;

[[nodiscard]] Superblock* vfs_alloc_superblock() noexcept;
void vfs_free_superblock(Superblock* sb) noexcept;

[[nodiscard]] Inode* vfs_alloc_inode(Superblock* sb) noexcept;
void inode_get(Inode* inode) noexcept;
void inode_put(Inode* inode) noexcept;

[[nodiscard]] Dentry* vfs_alloc_dentry(Superblock* sb, Dentry* parent, StringView name) noexcept;
void dentry_get(Dentry* dentry) noexcept;
void dentry_put(Dentry* dentry) noexcept;
void dentry_attach(Dentry* parent, Dentry* child) noexcept;
void dentry_detach(Dentry* child) noexcept;
[[nodiscard]] Dentry* dentry_find_child(Dentry* parent, StringView name) noexcept;

[[nodiscard]] File* vfs_alloc_file() noexcept;
void file_get(File* file) noexcept;
void file_put(File* file) noexcept;

// Mount table management.
[[nodiscard]] int32_t vfs_mount_root(Superblock* root_sb) noexcept;
[[nodiscard]] int32_t vfs_mount(StringView target_path, Superblock* sb) noexcept;
[[nodiscard]] int32_t vfs_umount(StringView target_path) noexcept;
[[nodiscard]] Dentry* vfs_root_dentry() noexcept;

// Path resolution (namei).
[[nodiscard]] int32_t vfs_namei(StringView path, Dentry* cwd, Dentry** out_dentry,
                                bool follow_symlinks = true) noexcept;
[[nodiscard]] int32_t vfs_namei_parent(StringView path, Dentry* cwd, Dentry** out_parent,
                                       StringView* out_leaf,
                                       bool* out_trailing_slash = nullptr) noexcept;
[[nodiscard]] int32_t vfs_readlink(StringView path, Dentry* cwd, char* buf, size_t buf_size,
                                   size_t* out_len) noexcept;

// High-level VFS file & namespace operations.
[[nodiscard]] int32_t vfs_open_at(Dentry* cwd, StringView path, uint32_t flags, uint32_t mode,
                                  File** out_file) noexcept;
[[nodiscard]] int32_t vfs_open(StringView path, uint32_t flags, uint32_t mode,
                               File** out_file) noexcept;
[[nodiscard]] int32_t vfs_close(File* file) noexcept;
[[nodiscard]] int64_t vfs_read(File* file, void* buf, size_t count) noexcept;
[[nodiscard]] int64_t vfs_write(File* file, const void* buf, size_t count) noexcept;
[[nodiscard]] int64_t vfs_lseek(File* file, int64_t offset, SeekWhence whence) noexcept;
[[nodiscard]] int32_t vfs_ioctl(File* file, uint64_t request, uint64_t arg) noexcept;
[[nodiscard]] int16_t vfs_poll(File* file, int16_t events) noexcept;
[[nodiscard]] int32_t vfs_readdir(File* file, DirEntryInfo* out_entry) noexcept;
[[nodiscard]] int32_t vfs_mkdir_at(Dentry* cwd, StringView path, uint32_t mode) noexcept;
[[nodiscard]] int32_t vfs_mkdir(StringView path, uint32_t mode) noexcept;
[[nodiscard]] int32_t vfs_unlink_at(Dentry* cwd, StringView path) noexcept;
[[nodiscard]] int32_t vfs_unlink(StringView path) noexcept;
[[nodiscard]] int32_t vfs_rmdir_at(Dentry* cwd, StringView path) noexcept;
[[nodiscard]] int32_t vfs_rmdir(StringView path) noexcept;
[[nodiscard]] int32_t vfs_rename_at(Dentry* old_cwd, StringView old_path, Dentry* new_cwd,
                                    StringView new_path) noexcept;
[[nodiscard]] int32_t vfs_rename(StringView old_path, StringView new_path) noexcept;

} // namespace Kernel::FS
