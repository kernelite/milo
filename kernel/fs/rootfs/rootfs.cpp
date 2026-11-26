#include "fs/rootfs/rootfs.hpp"

#include "fs/cpio.hpp"
#include "mm/address_space.hpp"

namespace Kernel::FS {

namespace {

struct RootfsPayload {
    const uint8_t* data{nullptr};
    size_t size{0U};
    bool active{false};
};

RootfsPayload g_rootfs_payloads[kMaxInodes]{};
uint64_t g_next_rootfs_ino = 2ULL;

[[nodiscard]] RootfsPayload* alloc_rootfs_payload(const uint8_t* data, size_t size) noexcept {
    for (auto& slot : g_rootfs_payloads) {
        if (!slot.active) {
            slot = RootfsPayload{
                .data = data,
                .size = size,
                .active = true,
            };
            return &slot;
        }
    }
    return nullptr;
}

void rootfs_destroy_inode(Inode* inode) noexcept {
    if (inode == nullptr) {
        return;
    }
    if (inode->fs_private != nullptr) {
        auto* payload = static_cast<RootfsPayload*>(inode->fs_private);
        *payload = RootfsPayload{};
        inode->fs_private = nullptr;
    }
    *inode = Inode{};
}

int64_t rootfs_read(File* file, void* buf, size_t count, uint64_t* offset) noexcept {
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
    const auto* payload = static_cast<const RootfsPayload*>(file->inode->fs_private);
    if (payload == nullptr || payload->data == nullptr || *offset >= payload->size) {
        return 0;
    }
    const size_t avail = payload->size - static_cast<size_t>(*offset);
    const size_t to_copy = (count < avail) ? count : avail;
    auto* dst = static_cast<uint8_t*>(buf);
    const uint8_t* src = payload->data + *offset;
    for (size_t idx = 0U; idx < to_copy; ++idx) {
        dst[idx] = src[idx];
    }
    *offset += to_copy;
    return static_cast<int64_t>(to_copy);
}

int64_t rootfs_write(File* /*file*/, const void* /*buf*/, size_t /*count*/,
                     uint64_t* /*offset*/) noexcept {
    return kErofs;
}

int64_t rootfs_lseek(File* file, int64_t offset, SeekWhence whence) noexcept {
    if (file == nullptr || !file->active || file->inode == nullptr) {
        return kEbadf;
    }
    int64_t base = 0;
    switch (whence) {
    case SeekWhence::kSet:
        base = 0;
        break;
    case SeekWhence::kCur:
        base = static_cast<int64_t>(file->offset);
        break;
    case SeekWhence::kEnd:
        base = static_cast<int64_t>(file->inode->size);
        break;
    default:
        return kEinval;
    }
    const int64_t target = base + offset;
    if (target < 0) {
        return kEinval;
    }
    file->offset = static_cast<uint64_t>(target);
    return target;
}

int32_t rootfs_readdir(File* file, DirEntryInfo* out_entry) noexcept {
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

int32_t rootfs_readlink(Inode* inode, char* buf, size_t buf_size, size_t* out_len) noexcept {
    if (inode == nullptr || buf == nullptr || buf_size == 0U || out_len == nullptr) {
        return kEinval;
    }
    if (!inode->is_symlink()) {
        return kEinval;
    }
    const auto* payload = static_cast<const RootfsPayload*>(inode->fs_private);
    if (payload == nullptr || (payload->size > 0U && payload->data == nullptr)) {
        return kEio;
    }
    const size_t copy_len = (payload->size < buf_size) ? payload->size : buf_size;
    for (size_t idx = 0U; idx < copy_len; ++idx) {
        buf[idx] = static_cast<char>(payload->data[idx]);
    }
    *out_len = copy_len;
    return kOk;
}

int32_t rootfs_create_ro(Inode* /*dir*/, Dentry* /*target*/, uint32_t /*mode*/) noexcept {
    return kErofs;
}

int32_t rootfs_mkdir_ro(Inode* /*dir*/, Dentry* /*target*/, uint32_t /*mode*/) noexcept {
    return kErofs;
}

int32_t rootfs_unlink_ro(Inode* /*dir*/, Dentry* /*target*/) noexcept {
    return kErofs;
}

int32_t rootfs_rmdir_ro(Inode* /*dir*/, Dentry* /*target*/) noexcept {
    return kErofs;
}

int32_t rootfs_rename_ro(Inode* /*old_dir*/, Dentry* /*old_dentry*/, Inode* /*new_dir*/,
                         Dentry* /*new_dentry*/) noexcept {
    return kErofs;
}

int32_t rootfs_truncate_ro(Inode* /*inode*/, uint64_t /*new_size*/) noexcept {
    return kErofs;
}

const SuperblockOperations kRootfsSbOps{
    .sync_fs = nullptr,
    .alloc_inode = nullptr,
    .destroy_inode = rootfs_destroy_inode,
};

const InodeOperations kRootfsInodeOps{
    .lookup = nullptr,
    .create = rootfs_create_ro,
    .mkdir = rootfs_mkdir_ro,
    .unlink = rootfs_unlink_ro,
    .rmdir = rootfs_rmdir_ro,
    .rename = rootfs_rename_ro,
    .truncate = rootfs_truncate_ro,
    .readlink = rootfs_readlink,
};

const FileOperations kRootfsFileOps{
    .read = rootfs_read,
    .write = rootfs_write,
    .open = nullptr,
    .release = nullptr,
    .lseek = rootfs_lseek,
    .ioctl = nullptr,
    .fsync = nullptr,
    .readdir = rootfs_readdir,
    .poll = nullptr,
};

[[nodiscard]] Inode* make_rootfs_inode(Superblock* superblock, uint32_t mode, const uint8_t* data,
                                       size_t size) noexcept {
    Inode* inode = vfs_alloc_inode(superblock);
    if (inode == nullptr) {
        return nullptr;
    }
    inode->ino = g_next_rootfs_ino++;
    inode->mode = mode;
    inode->size = size;
    inode->nlink = ((mode & kModeTypeMask) == kModeDir) ? 2U : 1U;
    inode->i_ops = &kRootfsInodeOps;
    inode->f_ops = &kRootfsFileOps;

    if ((mode & kModeTypeMask) != kModeDir) {
        RootfsPayload* payload = alloc_rootfs_payload(data, size);
        if (payload == nullptr) {
            inode_put(inode);
            return nullptr;
        }
        inode->fs_private = payload;
    }
    return inode;
}

[[nodiscard]] int32_t ensure_dir_path(Superblock* superblock, Dentry* root_dentry,
                                      StringView rel_path, Dentry** out_dir) noexcept {
    Dentry* curr = root_dentry;
    size_t pos = 0U;
    while (pos < rel_path.size()) {
        while (pos < rel_path.size() && rel_path[pos] == '/') {
            ++pos;
        }
        if (pos >= rel_path.size()) {
            break;
        }
        const size_t start = pos;
        while (pos < rel_path.size() && rel_path[pos] != '/') {
            ++pos;
        }
        const StringView comp = rel_path.substr(start, pos - start);
        if (comp == "." || comp.empty()) {
            continue;
        }
        if (comp.size() > kMaxNameLen) {
            return kEnametoolong;
        }

        Dentry* child = dentry_find_child(curr, comp);
        if (child == nullptr) {
            Inode* dir_ino = make_rootfs_inode(superblock, kModeDir | 0755U, nullptr, 0U);
            if (dir_ino == nullptr) {
                return kEnomem;
            }
            child = vfs_alloc_dentry(superblock, curr, comp);
            if (child == nullptr) {
                inode_put(dir_ino);
                return kEnomem;
            }
            child->inode = dir_ino;
            dentry_attach(curr, child);
            if (curr->inode != nullptr) {
                ++curr->inode->nlink;
            }
        } else if (child->inode == nullptr || !child->inode->is_dir()) {
            return kEnotdir;
        }
        curr = child;
    }

    if (out_dir != nullptr) {
        *out_dir = curr;
    }
    return kOk;
}

[[nodiscard]] int32_t insert_cpio_entry(Superblock* superblock, Dentry* root_dentry,
                                        const CpioEntry& entry) noexcept {
    const char* norm = cpio_normalize_path(entry.name);
    const StringView rel_path(norm);
    if (rel_path.empty() || rel_path == ".") {
        if (root_dentry->inode != nullptr && entry.is_directory) {
            root_dentry->inode->mode = kModeDir | (entry.mode & 07777U);
        }
        return kOk;
    }

    size_t end = rel_path.size();
    while (end > 0U && rel_path[end - 1U] == '/') {
        --end;
    }
    if (end == 0U) {
        return kOk;
    }

    const StringView trimmed = rel_path.substr(0U, end);
    const size_t last_slash = trimmed.rfind('/', end - 1U);
    StringView parent_rel{};
    StringView leaf{};
    if (last_slash == StringView::kNpos) {
        leaf = trimmed;
    } else {
        parent_rel = trimmed.substr(0U, last_slash);
        leaf = trimmed.substr(last_slash + 1U, end - (last_slash + 1U));
    }

    if (leaf.empty() || leaf == ".") {
        return kOk;
    }
    if (leaf.size() > kMaxNameLen) {
        return kEnametoolong;
    }

    Dentry* parent = root_dentry;
    const int32_t parent_res = ensure_dir_path(superblock, root_dentry, parent_rel, &parent);
    if (parent_res != kOk) {
        return parent_res;
    }

    Dentry* existing = dentry_find_child(parent, leaf);
    if (existing != nullptr) {
        if (entry.is_directory && existing->inode != nullptr && existing->inode->is_dir()) {
            existing->inode->mode = kModeDir | (entry.mode & 07777U);
        }
        return kOk;
    }

    Inode* inode = make_rootfs_inode(superblock, entry.mode, entry.data, entry.size);
    if (inode == nullptr) {
        return kEnomem;
    }

    Dentry* child = vfs_alloc_dentry(superblock, parent, leaf);
    if (child == nullptr) {
        inode_put(inode);
        return kEnomem;
    }
    child->inode = inode;
    dentry_attach(parent, child);
    if (entry.is_directory && parent->inode != nullptr) {
        ++parent->inode->nlink;
    }
    return kOk;
}

} // namespace

void rootfs_reset() noexcept {
    for (auto& slot : g_rootfs_payloads) {
        slot = RootfsPayload{};
    }
    g_next_rootfs_ino = 2ULL;
}

int32_t rootfs_create_from_cpio(const void* cpio_base, size_t cpio_size,
                                Superblock** out_sb) noexcept {
    if (out_sb == nullptr) {
        return kEinval;
    }
    *out_sb = nullptr;

    Superblock* superblock = vfs_alloc_superblock();
    if (superblock == nullptr) {
        return kEnomem;
    }
    superblock->fs_type = "rootfs";
    superblock->block_size = 4096U;
    superblock->flags = kSbReadOnly;
    superblock->ops = &kRootfsSbOps;

    Inode* root_inode = vfs_alloc_inode(superblock);
    if (root_inode == nullptr) {
        vfs_free_superblock(superblock);
        return kEnomem;
    }
    root_inode->ino = 1ULL;
    root_inode->mode = kModeDir | 0755U;
    root_inode->nlink = 2U;
    root_inode->i_ops = &kRootfsInodeOps;
    root_inode->f_ops = &kRootfsFileOps;

    Dentry* root_dentry = vfs_alloc_dentry(superblock, nullptr, "/");
    if (root_dentry == nullptr) {
        inode_put(root_inode);
        vfs_free_superblock(superblock);
        return kEnomem;
    }
    root_dentry->inode = root_inode;
    superblock->root = root_dentry;

    if (cpio_base != nullptr && cpio_size > 0U) {
        const CpioArchive archive(cpio_base, cpio_size);
        if (!archive.is_valid()) {
            dentry_put(root_dentry);
            vfs_free_superblock(superblock);
            return kEinval;
        }

        int32_t build_err = kOk;
        const CpioStatus status = archive.for_each([&](const CpioEntry& entry) -> bool {
            const int32_t insert_res = insert_cpio_entry(superblock, root_dentry, entry);
            if (insert_res != kOk) {
                build_err = insert_res;
                return false;
            }
            return true;
        });

        if (build_err != kOk) {
            return build_err;
        }
        if (status != CpioStatus::Success) {
            return kEinval;
        }
    }

    // Ensure standard UNIX mountpoint directories exist on rootfs.
    (void)ensure_dir_path(superblock, root_dentry, "dev", nullptr);
    (void)ensure_dir_path(superblock, root_dentry, "proc", nullptr);
    (void)ensure_dir_path(superblock, root_dentry, "tmp", nullptr);
    (void)ensure_dir_path(superblock, root_dentry, "mnt/disk", nullptr);

    *out_sb = superblock;
    return kOk;
}

int32_t rootfs_mount_cpio(const void* cpio_base, size_t cpio_size) noexcept {
    Superblock* superblock = nullptr;
    const int32_t create_res = rootfs_create_from_cpio(cpio_base, cpio_size, &superblock);
    if (create_res != kOk) {
        return create_res;
    }
    return vfs_mount_root(superblock);
}

int32_t rootfs_mount_from_initrd(const Kernel::Boot::InitrdLocation& initrd) noexcept {
    if (!initrd.valid || initrd.start_paddr == 0U || initrd.size == 0U) {
        return rootfs_mount_cpio(nullptr, 0U);
    }
    const auto* const initrd_kva =
        reinterpret_cast<const void*>(Kernel::MM::phys_to_virt(initrd.start_paddr));
    return rootfs_mount_cpio(initrd_kva, initrd.size);
}

} // namespace Kernel::FS