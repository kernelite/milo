#include "fs/Vfs.hpp"

namespace Kernel::FS {

namespace {

constexpr int16_t kPollInMask = 0x0041;  // POLLIN | POLLRDNORM
constexpr int16_t kPollOutMask = 0x0104; // POLLOUT | POLLWRNORM
constexpr int16_t kPollNval = 0x0020;

struct VfsState {
    Superblock superblocks[kMaxSuperblocks]{};
    Mount mounts[kMaxMounts]{};
    Inode inodes[kMaxInodes]{};
    Dentry dentries[kMaxDentries]{};
    File files[kMaxFiles]{};
    Mount* root_mount{nullptr};
};

VfsState g_vfs{};

[[nodiscard]] Mount* find_mount_by_mountpoint(const Dentry* mp) noexcept {
    if (mp == nullptr) {
        return nullptr;
    }
    for (auto& mnt : g_vfs.mounts) {
        if (mnt.is_active() && mnt.get_mountpoint() == mp) {
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
        if (mnt.is_active() && mnt.get_sb() != nullptr && mnt.get_sb()->get_root() == root) {
            return &mnt;
        }
    }
    return nullptr;
}

[[nodiscard]] Mount* resolve_enclosing_mount(const Dentry* start) noexcept {
    const Dentry* probe = start;
    while (probe != nullptr) {
        Mount* const mnt = find_mount_by_root(probe);
        if (mnt != nullptr) {
            return mnt;
        }
        probe = probe->get_parent();
    }
    return g_vfs.root_mount;
}

[[nodiscard]] bool has_child_mounts(const Mount* parent_mnt) noexcept {
    if (parent_mnt == nullptr) {
        return false;
    }
    // NOLINTNEXTLINE(readability-use-anyofallof)
    for (const auto& mnt : g_vfs.mounts) {
        if (mnt.is_active() && mnt.get_parent_mount() == parent_mnt) {
            return true;
        }
    }
    return false;
}

void descend_mounts(RefPtr<Dentry>& curr, Mount*& curr_mount) noexcept {
    while (curr && curr->is_mounted()) {
        Mount* const mnt = find_mount_by_mountpoint(curr.get());
        if (mnt == nullptr || mnt->get_sb() == nullptr || mnt->get_sb()->get_root() == nullptr) {
            break;
        }
        curr = RefPtr<Dentry>::retain_from(mnt->get_sb()->get_root());
        curr_mount = mnt;
    }
}

void ascend_parent(RefPtr<Dentry>& curr, Mount*& curr_mount) noexcept {
    if (!curr) {
        return;
    }

    while (curr_mount != nullptr && curr_mount->get_sb() != nullptr &&
           curr.get() == curr_mount->get_sb()->get_root()) {
        if (curr_mount->get_mountpoint() == nullptr) {
            return;
        }
        Dentry* const mp = curr_mount->get_mountpoint();
        Mount* const parent_mnt = curr_mount->get_parent_mount();
        curr = RefPtr<Dentry>::retain_from(mp);
        curr_mount = parent_mnt;
    }

    if (curr->get_parent() != nullptr) {
        curr = RefPtr<Dentry>::retain_from(curr->get_parent());
    }
}

[[nodiscard]] int32_t lookup_component(Dentry* dir_dentry, StringView name,
                                       RefPtr<Dentry>& out_child) noexcept {
    out_child.reset();
    if (dir_dentry == nullptr || dir_dentry->get_inode() == nullptr) {
        return kEinval;
    }
    Inode* const dir_inode = dir_dentry->get_inode();
    if (!dir_inode->is_dir()) {
        return kEnotdir;
    }
    if (name.size() > kMaxNameLen) {
        return kEnametoolong;
    }

    if (Dentry* const cached = dir_dentry->find_child(name); cached != nullptr) {
        out_child = RefPtr<Dentry>::retain_from(cached);
        return kOk;
    }

    auto candidate = RefPtr<Dentry>::adopt(vfs_alloc_dentry(dir_inode->get_sb(), dir_dentry, name));
    if (!candidate) {
        return kEnomem;
    }

    const int32_t rc = dir_inode->lookup(candidate.get());
    if (rc != kOk || candidate->get_inode() == nullptr) {
        return (rc != kOk) ? rc : kEnoent;
    }

    Dentry* const attached = candidate.detach();
    dir_dentry->attach_child(attached);
    out_child = RefPtr<Dentry>::retain_from(attached);
    return kOk;
}

[[nodiscard]] bool is_ancestor_dentry(const Dentry* potential_ancestor,
                                      const Dentry* node) noexcept {
    const Dentry* cur = node;
    while (cur != nullptr) {
        if (cur == potential_ancestor) {
            return true;
        }
        cur = cur->get_parent();
    }
    return false;
}

[[nodiscard]] int32_t namei_depth(StringView path, Dentry* cwd, bool follow_symlinks, size_t depth,
                                  RefPtr<Dentry>& out_dentry) noexcept {
    out_dentry.reset();

    if (depth > kMaxSymlinkDepth) {
        return kEloop;
    }
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
        curr_mount = resolve_enclosing_mount(start);
    }

    if (start == nullptr || start->get_inode() == nullptr) {
        return kEnoent;
    }

    auto curr = RefPtr<Dentry>::retain_from(start);
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

        size_t lookahead = pos;
        while (lookahead < path.size() && path[lookahead] == '/') {
            ++lookahead;
        }
        const bool is_last_component = (lookahead >= path.size());
        const bool has_trailing_slash = (pos < path.size() && path[pos] == '/');

        if (curr->get_inode() == nullptr || !curr->get_inode()->is_dir()) {
            return kEnotdir;
        }

        if (token == ".") {
            continue;
        }
        if (token == "..") {
            ascend_parent(curr, curr_mount);
            continue;
        }

        RefPtr<Dentry> next;
        const int32_t rc = lookup_component(curr.get(), token, next);
        if (rc != kOk) {
            return rc;
        }

        Inode* const next_inode = next->get_inode();
        if (next_inode != nullptr && next_inode->is_symlink() &&
            (!is_last_component || follow_symlinks || has_trailing_slash)) {
            char target_buf[256]{};
            size_t target_len = 0U;
            const int32_t rl_rc =
                next_inode->readlink(target_buf, sizeof(target_buf) - 1U, &target_len);
            next.reset();
            if (rl_rc != kOk) {
                return rl_rc;
            }
            target_buf[target_len] = '\0';

            RefPtr<Dentry> symlink_resolved;
            const int32_t sym_rc = namei_depth(StringView(target_buf, target_len), curr.get(), true,
                                               depth + 1U, symlink_resolved);
            if (sym_rc != kOk) {
                return sym_rc;
            }
            curr = static_cast<RefPtr<Dentry>&&>(symlink_resolved);
            curr_mount = resolve_enclosing_mount(curr.get());
            continue;
        }

        curr = static_cast<RefPtr<Dentry>&&>(next);
        descend_mounts(curr, curr_mount);
    }

    if (path.back() == '/' && (curr->get_inode() == nullptr || !curr->get_inode()->is_dir())) {
        return kEnotdir;
    }

    out_dentry = static_cast<RefPtr<Dentry>&&>(curr);
    return kOk;
}

[[nodiscard]] int32_t namei_parent_ref(StringView path, Dentry* cwd, RefPtr<Dentry>& out_parent,
                                       StringView& out_leaf,
                                       bool* out_trailing_slash = nullptr) noexcept {
    out_parent.reset();
    out_leaf = {};
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

    RefPtr<Dentry> parent;
    const int32_t rc = namei_depth(parent_path, cwd, true, 0U, parent);
    if (rc != kOk) {
        return rc;
    }

    if (parent->get_inode() == nullptr || !parent->get_inode()->is_dir()) {
        return kEnotdir;
    }

    out_parent = static_cast<RefPtr<Dentry>&&>(parent);
    out_leaf = leaf;
    return kOk;
}

} // namespace

// --- Inode lifecycle methods ---

void Inode::retain() noexcept {
    if (m_active) {
        ++m_refcount;
    }
}

void Inode::release() noexcept {
    if (!m_active || m_refcount == 0U) {
        return;
    }
    --m_refcount;
    if (m_refcount == 0U) {
        if (m_sb != nullptr && m_sb->destroy_inode(this)) {
            return;
        }
        reset();
    }
}

// --- Dentry lifecycle & hierarchy methods ---

void Dentry::retain() noexcept {
    if (m_active) {
        ++m_refcount;
    }
}

void Dentry::release() noexcept {
    if (!m_active || m_refcount == 0U) {
        return;
    }
    --m_refcount;
    if (m_refcount == 0U) {
        if (m_inode != nullptr) {
            m_inode->release();
            m_inode = nullptr;
        }
        reset();
    }
}

void Dentry::attach_child(Dentry* child) noexcept {
    if (child == nullptr || child == this) {
        return;
    }
    child->m_parent = this;
    child->m_next_sibling = m_first_child;
    m_first_child = child;
}

void Dentry::detach() noexcept {
    if (m_parent == nullptr) {
        return;
    }
    Dentry* prev = nullptr;
    Dentry* cur = m_parent->m_first_child;
    while (cur != nullptr) {
        if (cur == this) {
            if (prev == nullptr) {
                m_parent->m_first_child = cur->m_next_sibling;
            } else {
                prev->m_next_sibling = cur->m_next_sibling;
            }
            m_next_sibling = nullptr;
            m_parent = nullptr;
            return;
        }
        prev = cur;
        cur = cur->m_next_sibling;
    }
}

Dentry* Dentry::find_child(StringView child_name) const noexcept {
    for (Dentry* cur = m_first_child; cur != nullptr; cur = cur->m_next_sibling) {
        if (cur->is_active() && cur->name_view() == child_name) {
            return cur;
        }
    }
    return nullptr;
}

// --- File lifecycle & I/O operation methods ---

void File::retain() noexcept {
    if (m_active) {
        ++m_refcount;
    }
}

void File::release() noexcept {
    if (!m_active || m_refcount == 0U) {
        return;
    }
    --m_refcount;
    if (m_refcount == 0U) {
        if (m_f_ops != nullptr && m_f_ops->release != nullptr) {
            (void)m_f_ops->release(m_inode, this);
        }
        if (m_dentry != nullptr) {
            m_dentry->release();
            m_dentry = nullptr;
        }
        if (m_inode != nullptr) {
            m_inode->release();
            m_inode = nullptr;
        }
        reset();
    }
}

int64_t File::read(void* buf, size_t count) noexcept {
    if (!m_active || m_inode == nullptr) {
        return kEbadf;
    }
    const uint32_t acc = m_flags & kOpenAccessMask;
    if (acc == kOpenWriteOnly) {
        return kEbadf;
    }
    if (m_inode->is_dir()) {
        return kEisdir;
    }
    if (m_f_ops == nullptr || m_f_ops->read == nullptr) {
        return kEinval;
    }
    return m_f_ops->read(this, buf, count, &m_offset);
}

int64_t File::write(const void* buf, size_t count) noexcept {
    if (!m_active || m_inode == nullptr) {
        return kEbadf;
    }
    const uint32_t acc = m_flags & kOpenAccessMask;
    if (acc == kOpenReadOnly) {
        return kEbadf;
    }
    if (m_inode->is_dir()) {
        return kEisdir;
    }
    if (m_inode->get_sb() != nullptr && m_inode->get_sb()->is_read_only()) {
        return kErofs;
    }
    if (m_f_ops == nullptr || m_f_ops->write == nullptr) {
        return kEinval;
    }
    if ((m_flags & kOpenAppend) != 0U) {
        m_offset = m_inode->get_size();
    }
    return m_f_ops->write(this, buf, count, &m_offset);
}

int64_t File::seek(int64_t delta, SeekWhence whence) noexcept {
    if (!m_active || m_inode == nullptr) {
        return kEbadf;
    }
    if (m_f_ops != nullptr && m_f_ops->lseek != nullptr) {
        return m_f_ops->lseek(this, delta, whence);
    }

    int64_t base = 0;
    switch (whence) {
    case SeekWhence::kSet:
        base = 0;
        break;
    case SeekWhence::kCur:
        base = static_cast<int64_t>(m_offset);
        break;
    case SeekWhence::kEnd:
        base = static_cast<int64_t>(m_inode->get_size());
        break;
    default:
        return kEinval;
    }

    const int64_t target = base + delta;
    if (target < 0) {
        return kEinval;
    }
    m_offset = static_cast<uint64_t>(target);
    return target;
}

int32_t File::ioctl(uint64_t request, uint64_t arg) noexcept {
    if (!m_active || m_inode == nullptr) {
        return kEbadf;
    }
    if (m_f_ops == nullptr || m_f_ops->ioctl == nullptr) {
        return kEnotty;
    }
    return m_f_ops->ioctl(this, request, arg);
}

int16_t File::poll(int16_t events) noexcept {
    if (!m_active || m_inode == nullptr) {
        return kPollNval;
    }
    if (m_f_ops != nullptr && m_f_ops->poll != nullptr) {
        return m_f_ops->poll(this, events);
    }
    const uint32_t acc = m_flags & kOpenAccessMask;
    const auto event_mask = static_cast<uint16_t>(events);
    uint16_t revents = 0U;
    if (acc == kOpenReadOnly || acc == kOpenReadWrite) {
        revents |= static_cast<uint16_t>(event_mask & static_cast<uint16_t>(kPollInMask));
    }
    if (acc == kOpenWriteOnly || acc == kOpenReadWrite) {
        revents |= static_cast<uint16_t>(event_mask & static_cast<uint16_t>(kPollOutMask));
    }
    return static_cast<int16_t>(revents);
}

int32_t File::readdir(DirEntryInfo* out_entry) noexcept {
    if (!m_active || m_inode == nullptr || out_entry == nullptr) {
        return kEbadf;
    }
    if (!m_inode->is_dir()) {
        return kEnotdir;
    }
    if (m_f_ops == nullptr || m_f_ops->readdir == nullptr) {
        return kEopnotsupp;
    }
    return m_f_ops->readdir(this, out_entry);
}

int32_t File::fsync(int32_t datasync) noexcept {
    if (!m_active || m_inode == nullptr) {
        return kEbadf;
    }
    if (m_f_ops != nullptr && m_f_ops->fsync != nullptr) {
        return m_f_ops->fsync(this, datasync);
    }
    if (m_inode->get_sb() != nullptr) {
        return m_inode->get_sb()->sync();
    }
    return kOk;
}

// --- VFS lifecycle, pools & namespace operations ---

void vfs_reset() noexcept {
    for (auto& sb : g_vfs.superblocks) {
        sb.reset();
    }
    for (auto& mnt : g_vfs.mounts) {
        mnt.reset();
    }
    for (auto& ino : g_vfs.inodes) {
        ino.reset();
    }
    for (auto& entry : g_vfs.dentries) {
        entry.reset();
    }
    for (auto& file : g_vfs.files) {
        file.reset();
    }
    g_vfs.root_mount = nullptr;
}

VfsStats vfs_stats() noexcept {
    VfsStats stats{};
    for (const auto& sb : g_vfs.superblocks) {
        if (sb.is_active()) {
            ++stats.active_superblocks;
        }
    }
    for (const auto& mnt : g_vfs.mounts) {
        if (mnt.is_active()) {
            ++stats.active_mounts;
        }
    }
    for (const auto& ino : g_vfs.inodes) {
        if (ino.is_active()) {
            ++stats.active_inodes;
        }
    }
    for (const auto& entry : g_vfs.dentries) {
        if (entry.is_active()) {
            ++stats.active_dentries;
        }
    }
    for (const auto& file : g_vfs.files) {
        if (file.is_active()) {
            ++stats.active_files;
        }
    }
    return stats;
}

Superblock* vfs_alloc_superblock() noexcept {
    for (auto& sb : g_vfs.superblocks) {
        if (!sb.is_active()) {
            sb.activate();
            return &sb;
        }
    }
    return nullptr;
}

void vfs_free_superblock(Superblock* sb) noexcept {
    if (sb == nullptr || !sb->is_active()) {
        return;
    }
    sb->reset();
}

Inode* vfs_alloc_inode(Superblock* sb) noexcept {
    if (sb != nullptr) {
        if (Inode* const custom = sb->alloc_inode(); custom != nullptr) {
            return custom;
        }
        if (sb->get_ops() != nullptr && sb->get_ops()->alloc_inode != nullptr) {
            return nullptr;
        }
    }
    for (auto& ino : g_vfs.inodes) {
        if (!ino.is_active()) {
            ino.activate(sb);
            ino.set_nlink(1U);
            return &ino;
        }
    }
    return nullptr;
}

Dentry* vfs_alloc_dentry(Superblock* sb, Dentry* parent, StringView name) noexcept {
    if (name.size() > kMaxNameLen) {
        return nullptr;
    }
    for (auto& entry : g_vfs.dentries) {
        if (!entry.is_active()) {
            entry.activate(sb, parent, name);
            return &entry;
        }
    }
    return nullptr;
}

File* vfs_alloc_file() noexcept {
    for (auto& file : g_vfs.files) {
        if (!file.is_active()) {
            file.activate();
            return &file;
        }
    }
    return nullptr;
}

Dentry* vfs_root_dentry() noexcept {
    if (g_vfs.root_mount == nullptr || g_vfs.root_mount->get_sb() == nullptr) {
        return nullptr;
    }
    return g_vfs.root_mount->get_sb()->get_root();
}

int32_t vfs_mount_root(Superblock* root_sb) noexcept {
    if (root_sb == nullptr || root_sb->get_root() == nullptr ||
        root_sb->get_root()->get_inode() == nullptr) {
        return kEinval;
    }
    if (!root_sb->get_root()->get_inode()->is_dir()) {
        return kEnotdir;
    }
    if (g_vfs.root_mount != nullptr) {
        return kEbusy;
    }

    for (auto& mnt : g_vfs.mounts) {
        if (!mnt.is_active()) {
            mnt.activate(nullptr, root_sb, nullptr);
            root_sb->get_root()->mark_root();
            g_vfs.root_mount = &mnt;
            return kOk;
        }
    }
    return kEnomem;
}

int32_t vfs_mount(StringView target_path, Superblock* sb) noexcept {
    if (sb == nullptr || sb->get_root() == nullptr || sb->get_root()->get_inode() == nullptr) {
        return kEinval;
    }
    if (!sb->get_root()->get_inode()->is_dir()) {
        return kEnotdir;
    }
    if (target_path == "/") {
        return vfs_mount_root(sb);
    }

    RefPtr<Dentry> target;
    const int32_t rc = namei_depth(target_path, nullptr, true, 0U, target);
    if (rc != kOk) {
        return rc;
    }

    if (target->get_inode() == nullptr || !target->get_inode()->is_dir()) {
        return kEnotdir;
    }
    if (target->is_mounted() || find_mount_by_root(target.get()) != nullptr) {
        return kEbusy;
    }

    Mount* const parent_mnt = resolve_enclosing_mount(target.get());

    for (auto& mnt : g_vfs.mounts) {
        if (!mnt.is_active()) {
            Dentry* const mp = target.detach();
            mnt.activate(mp, sb, parent_mnt);
            mp->set_mounted(true);
            sb->get_root()->mark_root();
            return kOk;
        }
    }

    return kEnomem;
}

int32_t vfs_umount(StringView target_path) noexcept {
    if (target_path.empty()) {
        return kEinval;
    }

    RefPtr<Dentry> resolved;
    const int32_t rc = namei_depth(target_path, nullptr, true, 0U, resolved);
    if (rc != kOk) {
        return rc;
    }

    Mount* const mnt = find_mount_by_root(resolved.get());
    resolved.reset();

    if (mnt == nullptr) {
        return kEinval;
    }
    if (has_child_mounts(mnt)) {
        return kEbusy;
    }

    Superblock* const sb = mnt->get_sb();
    if (sb != nullptr && sb->get_root() != nullptr && sb->get_root()->get_refcount() > 1U) {
        return kEbusy;
    }

    if (sb != nullptr) {
        (void)sb->sync();
    }

    if (Dentry* const mp = mnt->get_mountpoint(); mp != nullptr) {
        mp->set_mounted(false);
        mp->release();
    }
    if (mnt == g_vfs.root_mount) {
        g_vfs.root_mount = nullptr;
    }
    mnt->reset();
    return kOk;
}

int32_t vfs_namei(StringView path, Dentry* cwd, Dentry** out_dentry,
                  bool follow_symlinks) noexcept {
    if (out_dentry == nullptr) {
        return kEinval;
    }
    *out_dentry = nullptr;

    RefPtr<Dentry> resolved;
    const int32_t rc = namei_depth(path, cwd, follow_symlinks, 0U, resolved);
    if (rc != kOk) {
        return rc;
    }
    *out_dentry = resolved.detach();
    return kOk;
}

int32_t vfs_namei_parent(StringView path, Dentry* cwd, Dentry** out_parent, StringView* out_leaf,
                         bool* out_trailing_slash) noexcept {
    if (out_parent == nullptr || out_leaf == nullptr) {
        return kEinval;
    }
    *out_parent = nullptr;
    *out_leaf = {};

    RefPtr<Dentry> parent;
    StringView leaf{};
    const int32_t rc = namei_parent_ref(path, cwd, parent, leaf, out_trailing_slash);
    if (rc != kOk) {
        return rc;
    }

    *out_parent = parent.detach();
    *out_leaf = leaf;
    return kOk;
}

int32_t vfs_readlink(StringView path, Dentry* cwd, char* buf, size_t buf_size,
                     size_t* out_len) noexcept {
    if (buf == nullptr || buf_size == 0U || out_len == nullptr) {
        return kEinval;
    }
    *out_len = 0U;

    RefPtr<Dentry> target;
    const int32_t rc = namei_depth(path, cwd, false, 0U, target);
    if (rc != kOk) {
        return rc;
    }

    Inode* const inode = target->get_inode();
    if (inode == nullptr || !inode->is_symlink()) {
        return kEinval;
    }
    return inode->readlink(buf, buf_size, out_len);
}

int32_t vfs_open_at(Dentry* cwd, StringView path, uint32_t flags, uint32_t mode,
                    File** out_file) noexcept {
    if (out_file == nullptr) {
        return kEinval;
    }
    *out_file = nullptr;

    const uint32_t acc = flags & kOpenAccessMask;
    const bool wants_write = (acc == kOpenWriteOnly || acc == kOpenReadWrite);

    RefPtr<Dentry> target;
    int32_t rc = namei_depth(path, cwd, true, 0U, target);

    if (rc == kEnoent && (flags & kOpenCreate) != 0U) {
        RefPtr<Dentry> parent;
        StringView leaf{};
        bool trailing_slash = false;
        rc = namei_parent_ref(path, cwd, parent, leaf, &trailing_slash);
        if (rc != kOk) {
            return rc;
        }
        if (trailing_slash || leaf == "." || leaf == "..") {
            return kEinval;
        }
        Inode* const parent_inode = parent->get_inode();
        if (parent_inode->get_sb() != nullptr && parent_inode->get_sb()->is_read_only()) {
            return kErofs;
        }

        auto created =
            RefPtr<Dentry>::adopt(vfs_alloc_dentry(parent_inode->get_sb(), parent.get(), leaf));
        if (!created) {
            return kEnomem;
        }

        const uint32_t reg_mode = kModeReg | (mode & 07777U);
        rc = parent_inode->create(created.get(), reg_mode);
        if (rc != kOk || created->get_inode() == nullptr) {
            return (rc != kOk) ? rc : kEio;
        }

        Dentry* const attached = created.detach();
        parent->attach_child(attached);
        target = RefPtr<Dentry>::retain_from(attached);
    } else if (rc == kOk && (flags & kOpenCreate) != 0U && (flags & kOpenExcl) != 0U) {
        return kEexist;
    } else if (rc != kOk) {
        return rc;
    }

    Inode* const inode = target->get_inode();
    if (inode == nullptr) {
        return kEnoent;
    }

    if (inode->is_dir() &&
        (wants_write || (flags & kOpenTrunc) != 0U || (flags & kOpenCreate) != 0U)) {
        return kEisdir;
    }

    if ((flags & kOpenDirectory) != 0U && !inode->is_dir()) {
        return kEnotdir;
    }

    if ((wants_write || (flags & kOpenTrunc) != 0U) && inode->get_sb() != nullptr &&
        inode->get_sb()->is_read_only()) {
        return kErofs;
    }

    if ((flags & kOpenTrunc) != 0U && inode->is_reg()) {
        const int32_t trc = inode->truncate(0U);
        if (trc != kOk) {
            return trc;
        }
    }

    auto file = RefPtr<File>::adopt(vfs_alloc_file());
    if (!file) {
        return kEnfile;
    }

    inode->retain();
    file->set_inode(inode);
    file->set_dentry(target.detach());
    file->set_flags(flags);
    file->set_offset(((flags & kOpenAppend) != 0U) ? inode->get_size() : 0U);
    file->set_f_ops(inode->get_f_ops());

    if (file->get_f_ops() != nullptr && file->get_f_ops()->open != nullptr) {
        const int32_t orc = file->get_f_ops()->open(inode, file.get());
        if (orc != kOk) {
            return orc;
        }
    }

    *out_file = file.detach();
    return kOk;
}

int32_t vfs_open(StringView path, uint32_t flags, uint32_t mode, File** out_file) noexcept {
    return vfs_open_at(nullptr, path, flags, mode, out_file);
}

int32_t vfs_close(File* file) noexcept {
    if (file == nullptr || !file->is_active()) {
        return kEbadf;
    }
    file->release();
    return kOk;
}

int64_t vfs_read(File* file, void* buf, size_t count) noexcept {
    return (file != nullptr) ? file->read(buf, count) : kEbadf;
}

int64_t vfs_write(File* file, const void* buf, size_t count) noexcept {
    return (file != nullptr) ? file->write(buf, count) : kEbadf;
}

int64_t vfs_lseek(File* file, int64_t offset, SeekWhence whence) noexcept {
    return (file != nullptr) ? file->seek(offset, whence) : kEbadf;
}

int32_t vfs_ioctl(File* file, uint64_t request, uint64_t arg) noexcept {
    return (file != nullptr) ? file->ioctl(request, arg) : kEbadf;
}

int16_t vfs_poll(File* file, int16_t events) noexcept {
    return (file != nullptr) ? file->poll(events) : kPollNval;
}

int32_t vfs_readdir(File* file, DirEntryInfo* out_entry) noexcept {
    return (file != nullptr) ? file->readdir(out_entry) : kEbadf;
}

int32_t vfs_mkdir_at(Dentry* cwd, StringView path, uint32_t mode) noexcept {
    RefPtr<Dentry> parent;
    StringView leaf{};
    const int32_t rc = namei_parent_ref(path, cwd, parent, leaf);
    if (rc != kOk) {
        return rc;
    }
    if (leaf == "." || leaf == "..") {
        return kEexist;
    }
    Inode* const parent_inode = parent->get_inode();
    if (parent_inode->get_sb() != nullptr && parent_inode->get_sb()->is_read_only()) {
        return kErofs;
    }

    RefPtr<Dentry> existing;
    if (lookup_component(parent.get(), leaf, existing) == kOk) {
        return kEexist;
    }

    auto child =
        RefPtr<Dentry>::adopt(vfs_alloc_dentry(parent_inode->get_sb(), parent.get(), leaf));
    if (!child) {
        return kEnomem;
    }

    const uint32_t dir_mode = kModeDir | (mode & 07777U);
    const int32_t mrc = parent_inode->mkdir(child.get(), dir_mode);
    if (mrc != kOk || child->get_inode() == nullptr) {
        return (mrc != kOk) ? mrc : kEio;
    }

    parent->attach_child(child.detach());
    return kOk;
}

int32_t vfs_mkdir(StringView path, uint32_t mode) noexcept {
    return vfs_mkdir_at(nullptr, path, mode);
}

int32_t vfs_unlink_at(Dentry* cwd, StringView path) noexcept {
    RefPtr<Dentry> parent;
    StringView leaf{};
    bool trailing_slash = false;
    const int32_t rc = namei_parent_ref(path, cwd, parent, leaf, &trailing_slash);
    if (rc != kOk) {
        return rc;
    }
    if (trailing_slash) {
        return kEnotdir;
    }
    if (leaf == "." || leaf == "..") {
        return kEisdir;
    }
    Inode* const parent_inode = parent->get_inode();
    if (parent_inode->get_sb() != nullptr && parent_inode->get_sb()->is_read_only()) {
        return kErofs;
    }

    RefPtr<Dentry> target;
    const int32_t lrc = lookup_component(parent.get(), leaf, target);
    if (lrc != kOk) {
        return lrc;
    }

    if (target->get_inode() != nullptr && target->get_inode()->is_dir()) {
        return kEisdir;
    }
    if (target->is_mounted()) {
        return kEbusy;
    }

    const int32_t urc = parent_inode->unlink(target.get());
    if (urc == kOk) {
        target->detach();
        target->release(); // Drop tree ownership reference
    }
    return urc;
}

int32_t vfs_unlink(StringView path) noexcept {
    return vfs_unlink_at(nullptr, path);
}

int32_t vfs_rmdir_at(Dentry* cwd, StringView path) noexcept {
    RefPtr<Dentry> parent;
    StringView leaf{};
    const int32_t rc = namei_parent_ref(path, cwd, parent, leaf);
    if (rc != kOk) {
        return rc;
    }
    if (leaf == "." || leaf == "..") {
        return kEinval;
    }
    Inode* const parent_inode = parent->get_inode();
    if (parent_inode->get_sb() != nullptr && parent_inode->get_sb()->is_read_only()) {
        return kErofs;
    }

    RefPtr<Dentry> target;
    const int32_t lrc = lookup_component(parent.get(), leaf, target);
    if (lrc != kOk) {
        return lrc;
    }

    if (target->get_inode() == nullptr || !target->get_inode()->is_dir()) {
        return kEnotdir;
    }
    if (target->is_mounted() || target->is_root()) {
        return kEbusy;
    }
    if (target->has_children()) {
        return kEnotempty;
    }

    const int32_t rrc = parent_inode->rmdir(target.get());
    if (rrc == kOk) {
        target->detach();
        target->release(); // Drop tree ownership reference
    }
    return rrc;
}

int32_t vfs_rmdir(StringView path) noexcept {
    return vfs_rmdir_at(nullptr, path);
}

int32_t vfs_rename_at(Dentry* old_cwd, StringView old_path, Dentry* new_cwd,
                      StringView new_path) noexcept {
    RefPtr<Dentry> old_parent;
    StringView old_leaf{};
    const int32_t orc = namei_parent_ref(old_path, old_cwd, old_parent, old_leaf);
    if (orc != kOk) {
        return orc;
    }

    RefPtr<Dentry> new_parent;
    StringView new_leaf{};
    const int32_t nrc = namei_parent_ref(new_path, new_cwd, new_parent, new_leaf);
    if (nrc != kOk) {
        return nrc;
    }

    if (old_leaf == "." || old_leaf == ".." || new_leaf == "." || new_leaf == "..") {
        return kEinval;
    }

    Inode* const old_parent_inode = old_parent->get_inode();
    Inode* const new_parent_inode = new_parent->get_inode();
    if (old_parent_inode->get_sb() != new_parent_inode->get_sb()) {
        return kExdev;
    }

    if (old_parent_inode->get_sb() != nullptr && old_parent_inode->get_sb()->is_read_only()) {
        return kErofs;
    }

    RefPtr<Dentry> old_dentry;
    const int32_t lrc = lookup_component(old_parent.get(), old_leaf, old_dentry);
    if (lrc != kOk) {
        return lrc;
    }

    if (old_dentry->is_mounted() || old_dentry->is_root()) {
        return kEbusy;
    }

    if (is_ancestor_dentry(old_dentry.get(), new_parent.get())) {
        return kEinval;
    }

    RefPtr<Dentry> target_dentry;
    const bool target_exists = (lookup_component(new_parent.get(), new_leaf, target_dentry) == kOk);

    if (target_exists && target_dentry == old_dentry) {
        return kOk;
    }

    if (target_exists) {
        if (target_dentry->is_mounted() || target_dentry->is_root()) {
            return kEbusy;
        }
        const bool src_is_dir =
            (old_dentry->get_inode() != nullptr && old_dentry->get_inode()->is_dir());
        const bool dst_is_dir =
            (target_dentry->get_inode() != nullptr && target_dentry->get_inode()->is_dir());
        if (src_is_dir && !dst_is_dir) {
            return kEnotdir;
        }
        if (!src_is_dir && dst_is_dir) {
            return kEisdir;
        }
        if (dst_is_dir && target_dentry->has_children()) {
            return kEnotempty;
        }
    } else {
        target_dentry = RefPtr<Dentry>::adopt(
            vfs_alloc_dentry(new_parent_inode->get_sb(), new_parent.get(), new_leaf));
        if (!target_dentry) {
            return kEnomem;
        }
    }

    const int32_t rrc =
        old_parent_inode->rename(old_dentry.get(), new_parent_inode, target_dentry.get());
    if (rrc == kOk) {
        if (target_exists) {
            target_dentry->detach();
            target_dentry->release(); // Drop tree ownership reference
        }
        old_dentry->detach();
        old_dentry->set_name(new_leaf);
        new_parent->attach_child(old_dentry.get());
    }

    return rrc;
}

int32_t vfs_rename(StringView old_path, StringView new_path) noexcept {
    return vfs_rename_at(nullptr, old_path, nullptr, new_path);
}

} // namespace Kernel::FS
