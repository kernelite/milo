#include "fs/vfs.hpp"

namespace kernel::fs {

namespace {

struct VfsState {
    Superblock superblocks[kMaxSuperblocks]{};
    Mount mounts[kMaxMounts]{};
    Inode inodes[kMaxInodes]{};
    Dentry dentries[kMaxDentries]{};
    File files[kMaxFiles]{};
    Mount* root_mount{nullptr};
};

VfsState g_vfs{};

void copy_name(char* dst, size_t dst_cap, StringView src) noexcept {
    if (dst == nullptr || dst_cap == 0U) {
        return;
    }
    const size_t copy_len = (src.size() < (dst_cap - 1U)) ? src.size() : (dst_cap - 1U);
    for (size_t idx = 0U; idx < copy_len; ++idx) {
        dst[idx] = src[idx];
    }
    dst[copy_len] = '\0';
}

[[nodiscard]] Mount* find_mount_by_mountpoint(const Dentry* mp) noexcept {
    if (mp == nullptr) {
        return nullptr;
    }
    for (auto& mnt : g_vfs.mounts) {
        if (mnt.active && mnt.mountpoint == mp) {
            return &mnt;
        }
    }
    return nullptr;
}

[[nodiscard]] Mount* find_mount_by_root(const Dentry* root) noexcept {
    if (root == nullptr) {
        return nullptr;
    }
    for (auto& mnt : g_vfs.mounts) {
        if (mnt.active && mnt.sb != nullptr && mnt.sb->root == root) {
            return &mnt;
        }
    }
    return nullptr;
}

[[nodiscard]] bool has_child_mounts(const Mount* parent_mnt) noexcept {
    if (parent_mnt == nullptr) {
        return false;
    }
    for (const auto& mnt : g_vfs.mounts) {
        if (mnt.active && mnt.parent_mount == parent_mnt) {
            return true;
        }
    }
    return false;
}

void descend_mounts(Dentry*& curr, Mount*& curr_mount) noexcept {
    while (curr != nullptr && curr->is_mounted()) {
        Mount* mnt = find_mount_by_mountpoint(curr);
        if (mnt == nullptr || mnt->sb == nullptr || mnt->sb->root == nullptr) {
            break;
        }
        Dentry* next_root = mnt->sb->root;
        dentry_get(next_root);
        dentry_put(curr);
        curr = next_root;
        curr_mount = mnt;
    }
}

void ascend_parent(Dentry*& curr, Mount*& curr_mount) noexcept {
    if (curr == nullptr) {
        return;
    }

    while (curr_mount != nullptr && curr == curr_mount->sb->root) {
        if (curr_mount->mountpoint == nullptr) {
            return;
        }
        Dentry* mp = curr_mount->mountpoint;
        Mount* parent_mnt = curr_mount->parent_mount;
        dentry_get(mp);
        dentry_put(curr);
        curr = mp;
        curr_mount = parent_mnt;
    }

    if (curr->parent != nullptr) {
        Dentry* parent = curr->parent;
        dentry_get(parent);
        dentry_put(curr);
        curr = parent;
    }
}

[[nodiscard]] int32_t lookup_component(Dentry* dir_dentry, StringView name,
                                       Dentry** out_child) noexcept {
    if (dir_dentry == nullptr || dir_dentry->inode == nullptr || out_child == nullptr) {
        return kEinval;
    }
    if (!dir_dentry->inode->is_dir()) {
        return kEnotdir;
    }
    if (name.size() > kMaxNameLen) {
        return kEnametoolong;
    }

    Dentry* cached = dentry_find_child(dir_dentry, name);
    if (cached != nullptr) {
        dentry_get(cached);
        *out_child = cached;
        return kOk;
    }

    Inode* dir_inode = dir_dentry->inode;
    if (dir_inode->i_ops == nullptr || dir_inode->i_ops->lookup == nullptr) {
        return kEnoent;
    }

    Dentry* candidate = vfs_alloc_dentry(dir_inode->sb, dir_dentry, name);
    if (candidate == nullptr) {
        return kEnomem;
    }

    const int32_t rc = dir_inode->i_ops->lookup(dir_inode, candidate);
    if (rc != kOk || candidate->inode == nullptr) {
        dentry_put(candidate);
        return (rc != kOk) ? rc : kEnoent;
    }

    dentry_attach(dir_dentry, candidate);
    dentry_get(candidate);
    *out_child = candidate;
    return kOk;
}

[[nodiscard]] bool is_ancestor_dentry(const Dentry* potential_ancestor,
                                      const Dentry* node) noexcept {
    const Dentry* cur = node;
    while (cur != nullptr) {
        if (cur == potential_ancestor) {
            return true;
        }
        cur = cur->parent;
    }
    return false;
}

} // namespace

void vfs_reset() noexcept {
    g_vfs = VfsState{};
}

VfsStats vfs_stats() noexcept {
    VfsStats stats{};
    for (const auto& sb : g_vfs.superblocks) {
        if (sb.active) {
            ++stats.active_superblocks;
        }
    }
    for (const auto& mnt : g_vfs.mounts) {
        if (mnt.active) {
            ++stats.active_mounts;
        }
    }
    for (const auto& ino : g_vfs.inodes) {
        if (ino.active) {
            ++stats.active_inodes;
        }
    }
    for (const auto& entry : g_vfs.dentries) {
        if (entry.active) {
            ++stats.active_dentries;
        }
    }
    for (const auto& file : g_vfs.files) {
        if (file.active) {
            ++stats.active_files;
        }
    }
    return stats;
}

Superblock* vfs_alloc_superblock() noexcept {
    for (auto& sb : g_vfs.superblocks) {
        if (!sb.active) {
            sb = Superblock{};
            sb.active = true;
            return &sb;
        }
    }
    return nullptr;
}

void vfs_free_superblock(Superblock* sb) noexcept {
    if (sb == nullptr || !sb->active) {
        return;
    }
    *sb = Superblock{};
}

Inode* vfs_alloc_inode(Superblock* sb) noexcept {
    if (sb != nullptr && sb->ops != nullptr && sb->ops->alloc_inode != nullptr) {
        return sb->ops->alloc_inode(sb);
    }
    for (auto& ino : g_vfs.inodes) {
        if (!ino.active) {
            ino = Inode{};
            ino.sb = sb;
            ino.refcount = 1U;
            ino.nlink = 1U;
            ino.active = true;
            return &ino;
        }
    }
    return nullptr;
}

void inode_get(Inode* inode) noexcept {
    if (inode != nullptr && inode->active) {
        ++inode->refcount;
    }
}

void inode_put(Inode* inode) noexcept {
    if (inode == nullptr || !inode->active || inode->refcount == 0U) {
        return;
    }
    --inode->refcount;
    if (inode->refcount == 0U) {
        Superblock* sb = inode->sb;
        if (sb != nullptr && sb->ops != nullptr && sb->ops->destroy_inode != nullptr) {
            sb->ops->destroy_inode(inode);
            return;
        }
        *inode = Inode{};
    }
}

Dentry* vfs_alloc_dentry(Superblock* sb, Dentry* parent, StringView name) noexcept {
    if (name.size() > kMaxNameLen) {
        return nullptr;
    }
    for (auto& entry : g_vfs.dentries) {
        if (!entry.active) {
            entry = Dentry{};
            copy_name(entry.name, sizeof(entry.name), name);
            entry.name_len = static_cast<uint8_t>(name.size());
            entry.sb = sb;
            entry.parent = parent;
            entry.refcount = 1U;
            entry.active = true;
            return &entry;
        }
    }
    return nullptr;
}

void dentry_get(Dentry* dentry) noexcept {
    if (dentry != nullptr && dentry->active) {
        ++dentry->refcount;
    }
}

void dentry_put(Dentry* dentry) noexcept {
    if (dentry == nullptr || !dentry->active || dentry->refcount == 0U) {
        return;
    }
    --dentry->refcount;
    if (dentry->refcount == 0U) {
        if (dentry->inode != nullptr) {
            inode_put(dentry->inode);
            dentry->inode = nullptr;
        }
        *dentry = Dentry{};
    }
}

void dentry_attach(Dentry* parent, Dentry* child) noexcept {
    if (parent == nullptr || child == nullptr || parent == child) {
        return;
    }
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

void dentry_detach(Dentry* child) noexcept {
    if (child == nullptr || child->parent == nullptr) {
        return;
    }
    Dentry* parent = child->parent;
    Dentry* prev = nullptr;
    Dentry* cur = parent->first_child;
    while (cur != nullptr) {
        if (cur == child) {
            if (prev == nullptr) {
                parent->first_child = cur->next_sibling;
            } else {
                prev->next_sibling = cur->next_sibling;
            }
            child->next_sibling = nullptr;
            child->parent = nullptr;
            return;
        }
        prev = cur;
        cur = cur->next_sibling;
    }
}

Dentry* dentry_find_child(Dentry* parent, StringView name) noexcept {
    if (parent == nullptr) {
        return nullptr;
    }
    for (Dentry* cur = parent->first_child; cur != nullptr; cur = cur->next_sibling) {
        if (cur->active && cur->name_view() == name) {
            return cur;
        }
    }
    return nullptr;
}

File* vfs_alloc_file() noexcept {
    for (auto& file : g_vfs.files) {
        if (!file.active) {
            file = File{};
            file.refcount = 1U;
            file.active = true;
            return &file;
        }
    }
    return nullptr;
}

void file_get(File* file) noexcept {
    if (file != nullptr && file->active) {
        ++file->refcount;
    }
}

void file_put(File* file) noexcept {
    if (file == nullptr || !file->active || file->refcount == 0U) {
        return;
    }
    --file->refcount;
    if (file->refcount == 0U) {
        if (file->f_ops != nullptr && file->f_ops->release != nullptr) {
            (void)file->f_ops->release(file->inode, file);
        }
        if (file->dentry != nullptr) {
            dentry_put(file->dentry);
            file->dentry = nullptr;
        }
        if (file->inode != nullptr) {
            inode_put(file->inode);
            file->inode = nullptr;
        }
        *file = File{};
    }
}

Dentry* vfs_root_dentry() noexcept {
    if (g_vfs.root_mount == nullptr || g_vfs.root_mount->sb == nullptr) {
        return nullptr;
    }
    return g_vfs.root_mount->sb->root;
}

int32_t vfs_mount_root(Superblock* root_sb) noexcept {
    if (root_sb == nullptr || root_sb->root == nullptr || root_sb->root->inode == nullptr) {
        return kEinval;
    }
    if (!root_sb->root->inode->is_dir()) {
        return kEnotdir;
    }
    if (g_vfs.root_mount != nullptr) {
        return kEbusy;
    }

    for (auto& mnt : g_vfs.mounts) {
        if (!mnt.active) {
            mnt = Mount{};
            mnt.mountpoint = nullptr;
            mnt.sb = root_sb;
            mnt.parent_mount = nullptr;
            mnt.active = true;
            root_sb->root->flags |= kDentryRoot;
            root_sb->root->parent = nullptr;
            g_vfs.root_mount = &mnt;
            return kOk;
        }
    }
    return kEnomem;
}

int32_t vfs_mount(StringView target_path, Superblock* sb) noexcept {
    if (sb == nullptr || sb->root == nullptr || sb->root->inode == nullptr) {
        return kEinval;
    }
    if (!sb->root->inode->is_dir()) {
        return kEnotdir;
    }
    if (target_path == "/") {
        return vfs_mount_root(sb);
    }

    Dentry* target = nullptr;
    const int32_t rc = vfs_namei(target_path, nullptr, &target);
    if (rc != kOk) {
        return rc;
    }

    if (target->inode == nullptr || !target->inode->is_dir()) {
        dentry_put(target);
        return kEnotdir;
    }
    if (target->is_mounted() || find_mount_by_root(target) != nullptr) {
        dentry_put(target);
        return kEbusy;
    }

    Mount* parent_mnt = find_mount_by_root(target);
    if (parent_mnt == nullptr) {
        Dentry* probe = target;
        while (probe != nullptr) {
            Mount* mnt = find_mount_by_root(probe);
            if (mnt != nullptr) {
                parent_mnt = mnt;
                break;
            }
            probe = probe->parent;
        }
        if (parent_mnt == nullptr) {
            parent_mnt = g_vfs.root_mount;
        }
    }

    for (auto& mnt : g_vfs.mounts) {
        if (!mnt.active) {
            mnt = Mount{};
            mnt.mountpoint = target;
            mnt.sb = sb;
            mnt.parent_mount = parent_mnt;
            mnt.active = true;
            target->flags |= kDentryMounted;
            sb->root->flags |= kDentryRoot;
            return kOk;
        }
    }

    dentry_put(target);
    return kEnomem;
}

int32_t vfs_umount(StringView target_path) noexcept {
    if (target_path.empty()) {
        return kEinval;
    }

    Dentry* resolved = nullptr;
    const int32_t rc = vfs_namei(target_path, nullptr, &resolved);
    if (rc != kOk) {
        return rc;
    }

    Mount* mnt = find_mount_by_root(resolved);
    dentry_put(resolved);

    if (mnt == nullptr) {
        return kEinval;
    }
    if (has_child_mounts(mnt)) {
        return kEbusy;
    }

    if (mnt->sb != nullptr && mnt->sb->root != nullptr && mnt->sb->root->refcount > 1U) {
        return kEbusy;
    }

    if (mnt->sb != nullptr && mnt->sb->ops != nullptr && mnt->sb->ops->sync_fs != nullptr) {
        (void)mnt->sb->ops->sync_fs(mnt->sb);
    }

    if (mnt->mountpoint != nullptr) {
        mnt->mountpoint->flags &= ~kDentryMounted;
        dentry_put(mnt->mountpoint);
    }
    if (mnt == g_vfs.root_mount) {
        g_vfs.root_mount = nullptr;
    }
    *mnt = Mount{};
    return kOk;
}

int32_t vfs_namei(StringView path, Dentry* cwd, Dentry** out_dentry) noexcept {
    if (out_dentry == nullptr) {
        return kEinval;
    }
    *out_dentry = nullptr;

    if (path.empty()) {
        return kEnoent;
    }

    Dentry* start = nullptr;
    Mount* curr_mount = nullptr;

    if (path.front() == '/') {
        start = vfs_root_dentry();
        curr_mount = g_vfs.root_mount;
    } else {
        start = (cwd != nullptr) ? cwd : vfs_root_dentry();
        Dentry* probe = start;
        while (probe != nullptr) {
            Mount* mnt = find_mount_by_root(probe);
            if (mnt != nullptr) {
                curr_mount = mnt;
                break;
            }
            probe = probe->parent;
        }
        if (curr_mount == nullptr) {
            curr_mount = g_vfs.root_mount;
        }
    }

    if (start == nullptr || start->inode == nullptr) {
        return kEnoent;
    }

    Dentry* curr = start;
    dentry_get(curr);
    descend_mounts(curr, curr_mount);

    size_t pos = 0U;
    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == '/') {
            ++pos;
        }
        if (pos >= path.size()) {
            break;
        }

        const size_t start_tok = pos;
        while (pos < path.size() && path[pos] != '/') {
            ++pos;
        }
        const StringView token = path.substr(start_tok, pos - start_tok);

        if (curr->inode == nullptr || !curr->inode->is_dir()) {
            dentry_put(curr);
            return kEnotdir;
        }

        if (token == ".") {
            continue;
        }
        if (token == "..") {
            ascend_parent(curr, curr_mount);
            continue;
        }

        Dentry* next = nullptr;
        const int32_t rc = lookup_component(curr, token, &next);
        dentry_put(curr);
        if (rc != kOk) {
            return rc;
        }
        curr = next;
        descend_mounts(curr, curr_mount);
    }

    if (path.back() == '/' && (curr->inode == nullptr || !curr->inode->is_dir())) {
        dentry_put(curr);
        return kEnotdir;
    }

    *out_dentry = curr;
    return kOk;
}

int32_t vfs_namei_parent(StringView path, Dentry* cwd, Dentry** out_parent, StringView* out_leaf,
                         bool* out_trailing_slash) noexcept {
    if (out_parent == nullptr || out_leaf == nullptr) {
        return kEinval;
    }
    *out_parent = nullptr;
    *out_leaf = {};
    if (out_trailing_slash != nullptr) {
        *out_trailing_slash = false;
    }

    if (path.empty()) {
        return kEnoent;
    }

    bool trailing_slash = false;
    size_t end = path.size();
    while (end > 0U && path[end - 1U] == '/') {
        trailing_slash = true;
        --end;
    }
    if (out_trailing_slash != nullptr) {
        *out_trailing_slash = trailing_slash;
    }

    if (end == 0U) {
        return kEbusy;
    }

    const size_t last_slash = path.rfind('/', end - 1U);
    StringView parent_path{};
    StringView leaf{};

    if (last_slash == StringView::kNpos) {
        parent_path = ".";
        leaf = path.substr(0U, end);
    } else if (last_slash == 0U) {
        parent_path = "/";
        leaf = path.substr(1U, end - 1U);
    } else {
        parent_path = path.substr(0U, last_slash);
        leaf = path.substr(last_slash + 1U, end - (last_slash + 1U));
    }

    if (leaf.empty()) {
        return kEinval;
    }
    if (leaf.size() > kMaxNameLen) {
        return kEnametoolong;
    }

    Dentry* parent = nullptr;
    const int32_t rc = vfs_namei(parent_path, cwd, &parent);
    if (rc != kOk) {
        return rc;
    }

    if (parent->inode == nullptr || !parent->inode->is_dir()) {
        dentry_put(parent);
        return kEnotdir;
    }

    *out_parent = parent;
    *out_leaf = leaf;
    return kOk;
}

int32_t vfs_open(StringView path, uint32_t flags, uint32_t mode, File** out_file) noexcept {
    if (out_file == nullptr) {
        return kEinval;
    }
    *out_file = nullptr;

    const uint32_t acc = flags & kOpenAccessMask;
    const bool wants_write = (acc == kOpenWriteOnly || acc == kOpenReadWrite);

    Dentry* target = nullptr;
    int32_t rc = vfs_namei(path, nullptr, &target);

    if (rc == kEnoent && (flags & kOpenCreate) != 0U) {
        Dentry* parent = nullptr;
        StringView leaf{};
        bool trailing_slash = false;
        rc = vfs_namei_parent(path, nullptr, &parent, &leaf, &trailing_slash);
        if (rc != kOk) {
            return rc;
        }
        if (trailing_slash || leaf == "." || leaf == "..") {
            dentry_put(parent);
            return kEinval;
        }
        if (parent->inode->sb != nullptr && parent->inode->sb->is_read_only()) {
            dentry_put(parent);
            return kErofs;
        }
        if (parent->inode->i_ops == nullptr || parent->inode->i_ops->create == nullptr) {
            dentry_put(parent);
            return kEopnotsupp;
        }

        Dentry* created = vfs_alloc_dentry(parent->inode->sb, parent, leaf);
        if (created == nullptr) {
            dentry_put(parent);
            return kEnomem;
        }

        const uint32_t reg_mode = kModeReg | (mode & 07777U);
        rc = parent->inode->i_ops->create(parent->inode, created, reg_mode);
        if (rc != kOk || created->inode == nullptr) {
            dentry_put(created);
            dentry_put(parent);
            return (rc != kOk) ? rc : kEio;
        }

        dentry_attach(parent, created);
        dentry_get(created);
        target = created;
        dentry_put(parent);
    } else if (rc == kOk && (flags & kOpenCreate) != 0U && (flags & kOpenExcl) != 0U) {
        dentry_put(target);
        return kEexist;
    } else if (rc != kOk) {
        return rc;
    }

    Inode* inode = target->inode;
    if (inode == nullptr) {
        dentry_put(target);
        return kEnoent;
    }

    if ((flags & kOpenDirectory) != 0U && !inode->is_dir()) {
        dentry_put(target);
        return kEnotdir;
    }

    if (inode->is_dir() && (wants_write || (flags & kOpenTrunc) != 0U)) {
        dentry_put(target);
        return kEisdir;
    }

    if ((wants_write || (flags & kOpenTrunc) != 0U) && inode->sb != nullptr &&
        inode->sb->is_read_only()) {
        dentry_put(target);
        return kErofs;
    }

    if ((flags & kOpenTrunc) != 0U && inode->is_reg()) {
        if (inode->i_ops != nullptr && inode->i_ops->truncate != nullptr) {
            const int32_t trc = inode->i_ops->truncate(inode, 0U);
            if (trc != kOk) {
                dentry_put(target);
                return trc;
            }
        } else {
            inode->size = 0U;
        }
    }

    File* file = vfs_alloc_file();
    if (file == nullptr) {
        dentry_put(target);
        return kEnfile;
    }

    inode_get(inode);
    file->inode = inode;
    file->dentry = target;
    file->flags = flags;
    file->offset = ((flags & kOpenAppend) != 0U) ? inode->size : 0U;
    file->f_ops = inode->f_ops;

    if (file->f_ops != nullptr && file->f_ops->open != nullptr) {
        const int32_t orc = file->f_ops->open(inode, file);
        if (orc != kOk) {
            file_put(file);
            return orc;
        }
    }

    *out_file = file;
    return kOk;
}

int32_t vfs_close(File* file) noexcept {
    if (file == nullptr || !file->active) {
        return kEbadf;
    }
    file_put(file);
    return kOk;
}

int64_t vfs_read(File* file, void* buf, size_t count) noexcept {
    if (file == nullptr || !file->active || file->inode == nullptr) {
        return kEbadf;
    }
    const uint32_t acc = file->flags & kOpenAccessMask;
    if (acc == kOpenWriteOnly) {
        return kEbadf;
    }
    if (file->inode->is_dir()) {
        return kEisdir;
    }
    if (file->f_ops == nullptr || file->f_ops->read == nullptr) {
        return kEinval;
    }
    return file->f_ops->read(file, buf, count, &file->offset);
}

int64_t vfs_write(File* file, const void* buf, size_t count) noexcept {
    if (file == nullptr || !file->active || file->inode == nullptr) {
        return kEbadf;
    }
    const uint32_t acc = file->flags & kOpenAccessMask;
    if (acc == kOpenReadOnly) {
        return kEbadf;
    }
    if (file->inode->is_dir()) {
        return kEisdir;
    }
    if (file->inode->sb != nullptr && file->inode->sb->is_read_only()) {
        return kErofs;
    }
    if (file->f_ops == nullptr || file->f_ops->write == nullptr) {
        return kEinval;
    }
    if ((file->flags & kOpenAppend) != 0U) {
        file->offset = file->inode->size;
    }
    return file->f_ops->write(file, buf, count, &file->offset);
}

int64_t vfs_lseek(File* file, int64_t offset, SeekWhence whence) noexcept {
    if (file == nullptr || !file->active || file->inode == nullptr) {
        return kEbadf;
    }
    if (file->f_ops != nullptr && file->f_ops->lseek != nullptr) {
        return file->f_ops->lseek(file, offset, whence);
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

int32_t vfs_readdir(File* file, DirEntryInfo* out_entry) noexcept {
    if (file == nullptr || !file->active || file->inode == nullptr || out_entry == nullptr) {
        return kEbadf;
    }
    if (!file->inode->is_dir()) {
        return kEnotdir;
    }
    if (file->f_ops == nullptr || file->f_ops->readdir == nullptr) {
        return kEopnotsupp;
    }
    return file->f_ops->readdir(file, out_entry);
}

int32_t vfs_mkdir(StringView path, uint32_t mode) noexcept {
    Dentry* parent = nullptr;
    StringView leaf{};
    const int32_t rc = vfs_namei_parent(path, nullptr, &parent, &leaf);
    if (rc != kOk) {
        return rc;
    }
    if (leaf == "." || leaf == "..") {
        dentry_put(parent);
        return kEexist;
    }
    if (parent->inode->sb != nullptr && parent->inode->sb->is_read_only()) {
        dentry_put(parent);
        return kErofs;
    }

    Dentry* existing = nullptr;
    if (lookup_component(parent, leaf, &existing) == kOk) {
        dentry_put(existing);
        dentry_put(parent);
        return kEexist;
    }

    if (parent->inode->i_ops == nullptr || parent->inode->i_ops->mkdir == nullptr) {
        dentry_put(parent);
        return kEopnotsupp;
    }

    Dentry* child = vfs_alloc_dentry(parent->inode->sb, parent, leaf);
    if (child == nullptr) {
        dentry_put(parent);
        return kEnomem;
    }

    const uint32_t dir_mode = kModeDir | (mode & 07777U);
    const int32_t mrc = parent->inode->i_ops->mkdir(parent->inode, child, dir_mode);
    if (mrc != kOk || child->inode == nullptr) {
        dentry_put(child);
        dentry_put(parent);
        return (mrc != kOk) ? mrc : kEio;
    }

    dentry_attach(parent, child);
    dentry_put(parent);
    return kOk;
}

int32_t vfs_unlink(StringView path) noexcept {
    Dentry* parent = nullptr;
    StringView leaf{};
    bool trailing_slash = false;
    const int32_t rc = vfs_namei_parent(path, nullptr, &parent, &leaf, &trailing_slash);
    if (rc != kOk) {
        return rc;
    }
    if (trailing_slash) {
        dentry_put(parent);
        return kEnotdir;
    }
    if (leaf == "." || leaf == "..") {
        dentry_put(parent);
        return kEisdir;
    }
    if (parent->inode->sb != nullptr && parent->inode->sb->is_read_only()) {
        dentry_put(parent);
        return kErofs;
    }

    Dentry* target = nullptr;
    const int32_t lrc = lookup_component(parent, leaf, &target);
    if (lrc != kOk) {
        dentry_put(parent);
        return lrc;
    }

    if (target->inode != nullptr && target->inode->is_dir()) {
        dentry_put(target);
        dentry_put(parent);
        return kEisdir;
    }
    if (target->is_mounted()) {
        dentry_put(target);
        dentry_put(parent);
        return kEbusy;
    }
    if (parent->inode->i_ops == nullptr || parent->inode->i_ops->unlink == nullptr) {
        dentry_put(target);
        dentry_put(parent);
        return kEopnotsupp;
    }

    const int32_t urc = parent->inode->i_ops->unlink(parent->inode, target);
    if (urc == kOk) {
        dentry_detach(target);
        dentry_put(target);
    }
    dentry_put(target);
    dentry_put(parent);
    return urc;
}

int32_t vfs_rmdir(StringView path) noexcept {
    Dentry* parent = nullptr;
    StringView leaf{};
    const int32_t rc = vfs_namei_parent(path, nullptr, &parent, &leaf);
    if (rc != kOk) {
        return rc;
    }
    if (leaf == "." || leaf == "..") {
        dentry_put(parent);
        return kEinval;
    }
    if (parent->inode->sb != nullptr && parent->inode->sb->is_read_only()) {
        dentry_put(parent);
        return kErofs;
    }

    Dentry* target = nullptr;
    const int32_t lrc = lookup_component(parent, leaf, &target);
    if (lrc != kOk) {
        dentry_put(parent);
        return lrc;
    }

    if (target->inode == nullptr || !target->inode->is_dir()) {
        dentry_put(target);
        dentry_put(parent);
        return kEnotdir;
    }
    if (target->is_mounted() || target->is_root()) {
        dentry_put(target);
        dentry_put(parent);
        return kEbusy;
    }
    if (target->first_child != nullptr) {
        dentry_put(target);
        dentry_put(parent);
        return kEnotempty;
    }
    if (parent->inode->i_ops == nullptr || parent->inode->i_ops->rmdir == nullptr) {
        dentry_put(target);
        dentry_put(parent);
        return kEopnotsupp;
    }

    const int32_t rrc = parent->inode->i_ops->rmdir(parent->inode, target);
    if (rrc == kOk) {
        dentry_detach(target);
        dentry_put(target);
    }
    dentry_put(target);
    dentry_put(parent);
    return rrc;
}

int32_t vfs_rename(StringView old_path, StringView new_path) noexcept {
    Dentry* old_parent = nullptr;
    StringView old_leaf{};
    const int32_t orc = vfs_namei_parent(old_path, nullptr, &old_parent, &old_leaf);
    if (orc != kOk) {
        return orc;
    }

    Dentry* new_parent = nullptr;
    StringView new_leaf{};
    const int32_t nrc = vfs_namei_parent(new_path, nullptr, &new_parent, &new_leaf);
    if (nrc != kOk) {
        dentry_put(old_parent);
        return nrc;
    }

    if (old_leaf == "." || old_leaf == ".." || new_leaf == "." || new_leaf == "..") {
        dentry_put(new_parent);
        dentry_put(old_parent);
        return kEinval;
    }

    if (old_parent->inode->sb != new_parent->inode->sb) {
        dentry_put(new_parent);
        dentry_put(old_parent);
        return kExdev;
    }

    if (old_parent->inode->sb != nullptr && old_parent->inode->sb->is_read_only()) {
        dentry_put(new_parent);
        dentry_put(old_parent);
        return kErofs;
    }

    Dentry* old_dentry = nullptr;
    const int32_t lrc = lookup_component(old_parent, old_leaf, &old_dentry);
    if (lrc != kOk) {
        dentry_put(new_parent);
        dentry_put(old_parent);
        return lrc;
    }

    if (old_dentry->is_mounted() || old_dentry->is_root()) {
        dentry_put(old_dentry);
        dentry_put(new_parent);
        dentry_put(old_parent);
        return kEbusy;
    }

    if (is_ancestor_dentry(old_dentry, new_parent)) {
        dentry_put(old_dentry);
        dentry_put(new_parent);
        dentry_put(old_parent);
        return kEinval;
    }

    Dentry* existing_target = nullptr;
    const bool target_exists = (lookup_component(new_parent, new_leaf, &existing_target) == kOk);

    if (target_exists && existing_target == old_dentry) {
        dentry_put(existing_target);
        dentry_put(old_dentry);
        dentry_put(new_parent);
        dentry_put(old_parent);
        return kOk;
    }

    Dentry* target_dentry = existing_target;
    if (target_exists) {
        if (existing_target->is_mounted() || existing_target->is_root()) {
            dentry_put(existing_target);
            dentry_put(old_dentry);
            dentry_put(new_parent);
            dentry_put(old_parent);
            return kEbusy;
        }
        const bool src_is_dir = (old_dentry->inode != nullptr && old_dentry->inode->is_dir());
        const bool dst_is_dir =
            (existing_target->inode != nullptr && existing_target->inode->is_dir());
        if (src_is_dir && !dst_is_dir) {
            dentry_put(existing_target);
            dentry_put(old_dentry);
            dentry_put(new_parent);
            dentry_put(old_parent);
            return kEnotdir;
        }
        if (!src_is_dir && dst_is_dir) {
            dentry_put(existing_target);
            dentry_put(old_dentry);
            dentry_put(new_parent);
            dentry_put(old_parent);
            return kEisdir;
        }
        if (dst_is_dir && existing_target->first_child != nullptr) {
            dentry_put(existing_target);
            dentry_put(old_dentry);
            dentry_put(new_parent);
            dentry_put(old_parent);
            return kEnotempty;
        }
    } else {
        target_dentry = vfs_alloc_dentry(new_parent->inode->sb, new_parent, new_leaf);
        if (target_dentry == nullptr) {
            dentry_put(old_dentry);
            dentry_put(new_parent);
            dentry_put(old_parent);
            return kEnomem;
        }
    }

    if (old_parent->inode->i_ops == nullptr || old_parent->inode->i_ops->rename == nullptr) {
        dentry_put(target_dentry);
        dentry_put(old_dentry);
        dentry_put(new_parent);
        dentry_put(old_parent);
        return kEopnotsupp;
    }

    const int32_t rrc = old_parent->inode->i_ops->rename(old_parent->inode, old_dentry,
                                                         new_parent->inode, target_dentry);
    if (rrc == kOk) {
        if (target_exists) {
            dentry_detach(existing_target);
            dentry_put(existing_target);
        }
        dentry_detach(old_dentry);
        copy_name(old_dentry->name, sizeof(old_dentry->name), new_leaf);
        old_dentry->name_len = static_cast<uint8_t>(new_leaf.size());
        dentry_attach(new_parent, old_dentry);
    }

    dentry_put(target_dentry);
    dentry_put(old_dentry);
    dentry_put(new_parent);
    dentry_put(old_parent);
    return rrc;
}

} // namespace kernel::fs