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
};

} // namespace Kernel::FS
