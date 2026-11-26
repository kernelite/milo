#include "fs/tmpfs/tmpfs.hpp"

#include "hal/cpu.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"

#include <cstring>

namespace Kernel::FS {

namespace {

struct TmpfsInodeData {
    uintptr_t pages[kTmpfsMaxPagesPerFile]{};
    bool active{false};
};

TmpfsInodeData g_tmpfs_inodes[kTmpfsMaxInodes]{};
Superblock* g_tmpfs_sb = nullptr;
uint64_t g_next_tmpfs_ino = 2ULL;

[[nodiscard]] uint64_t current_time_ns() noexcept {
    const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
    const uint64_t ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
    if (freq == 0ULL) {
        return 0ULL;
    }
    const uint64_t secs = ticks / freq;
    const uint64_t rem = ticks % freq;
    return (secs * 1000000000ULL) + ((rem * 1000000000ULL) / freq);
}

[[nodiscard]] TmpfsInodeData* alloc_tmpfs_data() noexcept {
    for (auto& slot : g_tmpfs_inodes) {
        if (!slot.active) {
            slot = TmpfsInodeData{};
            slot.active = true;
            return &slot;
        }
    }
    return nullptr;
}

void release_all_pages(TmpfsInodeData* data) noexcept {
    if (data == nullptr) {
        return;
    }
    for (uintptr_t& paddr : data->pages) {
        if (paddr != 0U) {
            Kernel::MM::free_frame(paddr);
            paddr = 0U;
        }
    }
}

void tmpfs_destroy_inode(Inode* inode) noexcept {
    if (inode == nullptr) {
        return;
    }
    if (inode->fs_private != nullptr) {
        auto* data = static_cast<TmpfsInodeData*>(inode->fs_private);
        release_all_pages(data);
        *data = TmpfsInodeData{};
        inode->fs_private = nullptr;
    }
    *inode = Inode{};
}

int64_t tmpfs_read(File* file, void* buf, size_t count, uint64_t* offset) noexcept {
    if (file == nullptr || file->inode == nullptr || offset == nullptr) {
        return kEinval;
    }
    if (file->inode->is_dir()) {
        return kEisdir;
    }
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kEinval;
    }

    Inode* inode = file->inode;
    if (*offset >= inode->size) {
        return 0;
    }

    const auto* data = static_cast<const TmpfsInodeData*>(inode->fs_private);
    if (data == nullptr) {
        return kEio;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uint64_t avail = inode->size - *offset;
    const size_t to_read =
        (static_cast<uint64_t>(count) < avail) ? count : static_cast<size_t>(avail);

    auto* dst = static_cast<uint8_t*>(buf);
    size_t total_read = 0U;

    while (total_read < to_read) {
        const auto page_idx = static_cast<size_t>(*offset / page_size);
        const auto page_off = static_cast<size_t>(*offset % page_size);
        const size_t page_rem = page_size - page_off;
        const size_t rem = to_read - total_read;
        const size_t chunk = (rem < page_rem) ? rem : page_rem;

        if (page_idx >= kTmpfsMaxPagesPerFile || data->pages[page_idx] == 0U) {
            std::memset(dst + total_read, 0, chunk);
        } else {
            const auto* src =
                reinterpret_cast<const uint8_t*>(Kernel::MM::phys_to_virt(data->pages[page_idx]));
            std::memcpy(dst + total_read, src + page_off, chunk);
        }

        *offset += chunk;
        total_read += chunk;
    }

    inode->atime_ns = current_time_ns();
    return static_cast<int64_t>(total_read);
}

int64_t tmpfs_write(File* file, const void* buf, size_t count, uint64_t* offset) noexcept {
    if (file == nullptr || file->inode == nullptr || offset == nullptr) {
        return kEinval;
    }
    if (file->inode->is_dir()) {
        return kEisdir;
    }
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kEinval;
    }

    Inode* inode = file->inode;
    auto* data = static_cast<TmpfsInodeData*>(inode->fs_private);
    if (data == nullptr) {
        return kEio;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const auto* src = static_cast<const uint8_t*>(buf);
    size_t total_written = 0U;

    while (total_written < count) {
        const auto page_idx = static_cast<size_t>(*offset / page_size);
        if (page_idx >= kTmpfsMaxPagesPerFile) {
            if (total_written > 0U) {
                break;
            }
            return kEnospc;
        }

        if (data->pages[page_idx] == 0U) {
            const uintptr_t frame = Kernel::MM::alloc_frame();
            if (frame == 0U) {
                if (total_written > 0U) {
                    break;
                }
                return kEnomem;
            }
            auto* page_kva = reinterpret_cast<void*>(Kernel::MM::phys_to_virt(frame));
            std::memset(page_kva, 0, page_size);
            data->pages[page_idx] = frame;
        }

        const auto page_off = static_cast<size_t>(*offset % page_size);
        const size_t page_rem = page_size - page_off;
        const size_t rem = count - total_written;
        const size_t chunk = (rem < page_rem) ? rem : page_rem;

        auto* dst = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(data->pages[page_idx]));
        std::memcpy(dst + page_off, src + total_written, chunk);

        *offset += chunk;
        total_written += chunk;
    }

    if (*offset > inode->size) {
        inode->size = *offset;
    }
    const uint64_t now_ns = current_time_ns();
    inode->mtime_ns = now_ns;
    inode->ctime_ns = now_ns;
    return static_cast<int64_t>(total_written);
}

int32_t tmpfs_readdir(File* file, DirEntryInfo* out_entry) noexcept {
    if (file == nullptr || file->dentry == nullptr || out_entry == nullptr) {
        return kEinval;
    }
    uint64_t idx = 0U;
    for (Dentry* child = file->dentry->first_child; child != nullptr; child = child->next_sibling) {
        if (!child->active || child->inode == nullptr) {
            continue;
        }
        if (idx == file->offset) {
            out_entry->ino = child->inode->ino;
            out_entry->type = child->inode->mode & kModeTypeMask;
            for (size_t name_idx = 0U; name_idx <= kMaxNameLen; ++name_idx) {
                out_entry->name[name_idx] =
                    (name_idx < child->name_len) ? child->name[name_idx] : '\0';
            }
            ++file->offset;
            return kOk;
        }
        ++idx;
    }
    return kEnoent;
}

int32_t tmpfs_truncate(Inode* inode, uint64_t new_size) noexcept {
    if (inode == nullptr) {
        return kEinval;
    }
    if (inode->is_dir()) {
        return kEisdir;
    }

    auto* data = static_cast<TmpfsInodeData*>(inode->fs_private);
    if (data == nullptr) {
        return kEio;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uint64_t max_bytes = static_cast<uint64_t>(kTmpfsMaxPagesPerFile) * page_size;
    if (new_size > max_bytes) {
        return kEnospc;
    }

    if (new_size < inode->size) {
        const auto keep_pages = static_cast<size_t>((new_size + page_size - 1U) / page_size);
        for (size_t page_idx = keep_pages; page_idx < kTmpfsMaxPagesPerFile; ++page_idx) {
            if (data->pages[page_idx] != 0U) {
                Kernel::MM::free_frame(data->pages[page_idx]);
                data->pages[page_idx] = 0U;
            }
        }

        const auto tail_off = static_cast<size_t>(new_size % page_size);
        if (tail_off != 0U) {
            const auto tail_idx = static_cast<size_t>(new_size / page_size);
            if (tail_idx < kTmpfsMaxPagesPerFile && data->pages[tail_idx] != 0U) {
                auto* page_kva =
                    reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(data->pages[tail_idx]));
                std::memset(page_kva + tail_off, 0, page_size - tail_off);
            }
        }
    }

    inode->size = new_size;
    const uint64_t now_ns = current_time_ns();
    inode->mtime_ns = now_ns;
    inode->ctime_ns = now_ns;
    return kOk;
}

extern const InodeOperations kTmpfsInodeOps;
extern const FileOperations kTmpfsFileOps;

int32_t tmpfs_create(Inode* dir, Dentry* target, uint32_t mode) noexcept {
    if (dir == nullptr || target == nullptr || dir->sb == nullptr) {
        return kEinval;
    }

    TmpfsInodeData* data = alloc_tmpfs_data();
    if (data == nullptr) {
        return kEnomem;
    }

    Inode* inode = vfs_alloc_inode(dir->sb);
    if (inode == nullptr) {
        *data = TmpfsInodeData{};
        return kEnomem;
    }

    const uint64_t now_ns = current_time_ns();
    inode->ino = g_next_tmpfs_ino++;
    inode->mode = kModeReg | (mode & 07777U);
    inode->nlink = 1U;
    inode->size = 0U;
    inode->atime_ns = now_ns;
    inode->mtime_ns = now_ns;
    inode->ctime_ns = now_ns;
    inode->i_ops = &kTmpfsInodeOps;
    inode->f_ops = &kTmpfsFileOps;
    inode->fs_private = data;

    target->inode = inode;
    dir->mtime_ns = now_ns;
    dir->ctime_ns = now_ns;
    return kOk;
}

int32_t tmpfs_mkdir(Inode* dir, Dentry* target, uint32_t mode) noexcept {
    if (dir == nullptr || target == nullptr || dir->sb == nullptr) {
        return kEinval;
    }

    Inode* inode = vfs_alloc_inode(dir->sb);
    if (inode == nullptr) {
        return kEnomem;
    }

    const uint64_t now_ns = current_time_ns();
    inode->ino = g_next_tmpfs_ino++;
    inode->mode = kModeDir | (mode & 07777U);
    inode->nlink = 2U;
    inode->size = 0U;
    inode->atime_ns = now_ns;
    inode->mtime_ns = now_ns;
    inode->ctime_ns = now_ns;
    inode->i_ops = &kTmpfsInodeOps;
    inode->f_ops = &kTmpfsFileOps;

    target->inode = inode;
    ++dir->nlink;
    dir->mtime_ns = now_ns;
    dir->ctime_ns = now_ns;
    return kOk;
}

int32_t tmpfs_unlink(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || target->inode == nullptr) {
        return kEinval;
    }

    const uint64_t now_ns = current_time_ns();
    if (target->inode->nlink > 0U) {
        --target->inode->nlink;
    }
    target->inode->ctime_ns = now_ns;
    dir->mtime_ns = now_ns;
    dir->ctime_ns = now_ns;
    return kOk;
}

int32_t tmpfs_rmdir(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || target->inode == nullptr) {
        return kEinval;
    }

    const uint64_t now_ns = current_time_ns();
    if (dir->nlink > 0U) {
        --dir->nlink;
    }
    target->inode->nlink = 0U;
    target->inode->ctime_ns = now_ns;
    dir->mtime_ns = now_ns;
    dir->ctime_ns = now_ns;
    return kOk;
}

int32_t tmpfs_rename(Inode* old_dir, Dentry* old_dentry, Inode* new_dir,
                     Dentry* new_dentry) noexcept {
    if (old_dir == nullptr || old_dentry == nullptr || new_dir == nullptr) {
        return kEinval;
    }

    const uint64_t now_ns = current_time_ns();

    if (new_dentry != nullptr && new_dentry->inode != nullptr) {
        if (new_dentry->inode->is_dir() && new_dir->nlink > 0U) {
            --new_dir->nlink;
        }
        if (new_dentry->inode->nlink > 0U) {
            --new_dentry->inode->nlink;
        }
        new_dentry->inode->ctime_ns = now_ns;
    }

    if (old_dentry->inode != nullptr) {
        if (old_dentry->inode->is_dir() && old_dir != new_dir) {
            if (old_dir->nlink > 0U) {
                --old_dir->nlink;
            }
            ++new_dir->nlink;
        }
        old_dentry->inode->ctime_ns = now_ns;
    }

    old_dir->mtime_ns = now_ns;
    old_dir->ctime_ns = now_ns;
    new_dir->mtime_ns = now_ns;
    new_dir->ctime_ns = now_ns;
    return kOk;
}

const SuperblockOperations kTmpfsSbOps{
    .sync_fs = nullptr,
    .alloc_inode = nullptr,
    .destroy_inode = tmpfs_destroy_inode,
};

const InodeOperations kTmpfsInodeOps{
    .lookup = nullptr,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .rename = tmpfs_rename,
    .truncate = tmpfs_truncate,
    .readlink = nullptr,
};

const FileOperations kTmpfsFileOps{
    .read = tmpfs_read,
    .write = tmpfs_write,
    .open = nullptr,
    .release = nullptr,
    .lseek = nullptr,
    .ioctl = nullptr,
    .fsync = nullptr,
    .readdir = tmpfs_readdir,
    .poll = nullptr,
};

} // namespace

void tmpfs_reset() noexcept {
    for (auto& slot : g_tmpfs_inodes) {
        release_all_pages(&slot);
        slot = TmpfsInodeData{};
    }
    g_tmpfs_sb = nullptr;
    g_next_tmpfs_ino = 2ULL;
}

int32_t tmpfs_create_sb(Superblock** out_sb) noexcept {
    if (out_sb == nullptr) {
        return kEinval;
    }
    *out_sb = nullptr;

    Superblock* superblock = vfs_alloc_superblock();
    if (superblock == nullptr) {
        return kEnomem;
    }
    superblock->fs_type = "tmpfs";
    superblock->block_size = 4096U;
    superblock->flags = 0U;
    superblock->ops = &kTmpfsSbOps;

    Inode* root_inode = vfs_alloc_inode(superblock);
    if (root_inode == nullptr) {
        vfs_free_superblock(superblock);
        return kEnomem;
    }

    const uint64_t now_ns = current_time_ns();
    root_inode->ino = 1ULL;
    root_inode->mode = kModeDir | 01777U;
    root_inode->nlink = 2U;
    root_inode->atime_ns = now_ns;
    root_inode->mtime_ns = now_ns;
    root_inode->ctime_ns = now_ns;
    root_inode->i_ops = &kTmpfsInodeOps;
    root_inode->f_ops = &kTmpfsFileOps;

    Dentry* root_dentry = vfs_alloc_dentry(superblock, nullptr, "tmp");
    if (root_dentry == nullptr) {
        inode_put(root_inode);
        vfs_free_superblock(superblock);
        return kEnomem;
    }
    root_dentry->inode = root_inode;
    superblock->root = root_dentry;

    *out_sb = superblock;
    return kOk;
}

int32_t tmpfs_init_and_mount() noexcept {
    if (g_tmpfs_sb != nullptr) {
        return kEbusy;
    }

    Superblock* superblock = nullptr;
    const int32_t create_res = tmpfs_create_sb(&superblock);
    if (create_res != kOk) {
        return create_res;
    }

    const int32_t mount_res = vfs_mount("/tmp", superblock);
    if (mount_res != kOk) {
        dentry_put(superblock->root);
        vfs_free_superblock(superblock);
        return mount_res;
    }

    g_tmpfs_sb = superblock;
    return kOk;
}

} // namespace Kernel::FS