#pragma once

#include "fs/VfsTypes.hpp"

namespace Kernel::FS {

struct FileOperations {
    int64_t (*read)(File* file, void* buf, size_t count, uint64_t* offset){nullptr};
    int64_t (*write)(File* file, const void* buf, size_t count, uint64_t* offset){nullptr};
    int32_t (*open)(Inode* inode, File* file){nullptr};
    int32_t (*release)(Inode* inode, File* file){nullptr};
    int64_t (*lseek)(File* file, int64_t offset, SeekWhence whence){nullptr};
    int32_t (*ioctl)(File* file, uint64_t request, uint64_t arg){nullptr};
    int32_t (*fsync)(File* file, int32_t datasync){nullptr};
    int32_t (*readdir)(File* file, DirEntryInfo* out_entry){nullptr};
    int16_t (*poll)(File* file, int16_t events){nullptr};
};

struct File {
    Inode* inode{nullptr};
    Dentry* dentry{nullptr};
    uint64_t offset{0U};
    uint32_t flags{0U};
    uint32_t refcount{0U};
    const FileOperations* f_ops{nullptr};
    void* private_data{nullptr};
    bool active{false};

    void retain() noexcept;
    void release() noexcept;

    [[nodiscard]] constexpr Inode* get_inode() const noexcept { return inode; }
    [[nodiscard]] constexpr Dentry* get_dentry() const noexcept { return dentry; }

    [[nodiscard]] constexpr uint64_t get_offset() const noexcept { return offset; }
    constexpr void set_offset(uint64_t new_offset) noexcept { offset = new_offset; }

    [[nodiscard]] constexpr uint32_t get_flags() const noexcept { return flags; }
    constexpr void set_flags(uint32_t new_flags) noexcept { flags = new_flags; }
    constexpr void set_status_flags(uint32_t status_flags) noexcept {
        constexpr uint32_t kMutableStatusMask = kOpenNonBlock | kOpenAppend;
        flags = (flags & ~kMutableStatusMask) | (status_flags & kMutableStatusMask);
    }

    [[nodiscard]] constexpr const FileOperations* get_f_ops() const noexcept { return f_ops; }
    constexpr void set_f_ops(const FileOperations* new_ops) noexcept { f_ops = new_ops; }

    [[nodiscard]] constexpr void* get_private_data() const noexcept { return private_data; }
    constexpr void set_private_data(void* new_data) noexcept { private_data = new_data; }

    [[nodiscard]] constexpr uint32_t get_refcount() const noexcept { return refcount; }
    [[nodiscard]] constexpr bool is_active() const noexcept { return active; }

    [[nodiscard]] int64_t read(void* buf, size_t count) noexcept;
    [[nodiscard]] int64_t write(const void* buf, size_t count) noexcept;
    [[nodiscard]] int64_t seek(int64_t delta, SeekWhence whence) noexcept;
    [[nodiscard]] int32_t ioctl(uint64_t request, uint64_t arg) noexcept;
    [[nodiscard]] int16_t poll(int16_t events) noexcept;
    [[nodiscard]] int32_t readdir(DirEntryInfo* out_entry) noexcept;
    [[nodiscard]] int32_t fsync(int32_t datasync = 0) noexcept;
};

} // namespace Kernel::FS
