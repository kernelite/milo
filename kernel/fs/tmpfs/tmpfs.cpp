#include "fs/tmpfs/tmpfs.hpp"

#include "hal/cpu.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "mm/AddressSpace.hpp"
#include "mm/pfa.hpp"

#include <cstring>

namespace Kernel::FS {

namespace {

class TmpfsNode final {
  public:
    constexpr TmpfsNode() noexcept = default;
    ~TmpfsNode() noexcept { reset(); }

    TmpfsNode(const TmpfsNode&) = delete;
    TmpfsNode& operator=(const TmpfsNode&) = delete;
    TmpfsNode(TmpfsNode&&) = delete;
    TmpfsNode& operator=(TmpfsNode&&) = delete;

    [[nodiscard]] bool is_active() const noexcept { return m_active; }

    void activate() noexcept {
        release_all_pages();
        m_active = true;
    }

    void reset() noexcept {
        release_all_pages();
        m_active = false;
    }

    [[nodiscard]] uintptr_t page_at(size_t page_idx) const noexcept {
        return (page_idx < kTmpfsMaxPagesPerFile) ? m_pages[page_idx] : 0U;
    }

    [[nodiscard]] uintptr_t ensure_page(size_t page_idx, size_t page_size) noexcept {
        if (page_idx >= kTmpfsMaxPagesPerFile) {
            return 0U;
        }
        if (m_pages[page_idx] == 0U) {
            const uintptr_t frame = Kernel::MM::alloc_frame();
            if (frame == 0U) {
                return 0U;
            }
            auto* const page_kva = reinterpret_cast<void*>(Kernel::MM::phys_to_virt(frame));
            std::memset(page_kva, 0, page_size);
            m_pages[page_idx] = frame;
        }
        return m_pages[page_idx];
    }

    void truncate_pages(uint64_t new_size, size_t page_size) noexcept {
        const auto keep_pages = static_cast<size_t>((new_size + page_size - 1U) / page_size);
        for (size_t page_idx = keep_pages; page_idx < kTmpfsMaxPagesPerFile; ++page_idx) {
            if (m_pages[page_idx] != 0U) {
                Kernel::MM::free_frame(m_pages[page_idx]);
                m_pages[page_idx] = 0U;
            }
        }

        const auto tail_off = static_cast<size_t>(new_size % page_size);
        if (tail_off != 0U) {
            const auto tail_idx = static_cast<size_t>(new_size / page_size);
            if (tail_idx < kTmpfsMaxPagesPerFile && m_pages[tail_idx] != 0U) {
                auto* const page_kva =
                    reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(m_pages[tail_idx]));
                std::memset(page_kva + tail_off, 0, page_size - tail_off);
            }
        }
    }

  private:
    void release_all_pages() noexcept {
        for (uintptr_t& paddr : m_pages) {
            if (paddr != 0U) {
                Kernel::MM::free_frame(paddr);
                paddr = 0U;
            }
        }
    }

    uintptr_t m_pages[kTmpfsMaxPagesPerFile]{};
    bool m_active{false};
};

TmpfsNode g_tmpfs_nodes[kTmpfsMaxInodes]{};
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

[[nodiscard]] TmpfsNode* alloc_tmpfs_node() noexcept {
    for (auto& slot : g_tmpfs_nodes) {
        if (!slot.is_active()) {
            slot.activate();
            return &slot;
        }
    }
    return nullptr;
}

void tmpfs_destroy_inode(Inode* inode) noexcept {
    if (inode == nullptr) {
        return;
    }
    if (inode->get_fs_private() != nullptr) {
        auto* const node = static_cast<TmpfsNode*>(inode->get_fs_private());
        node->reset();
        inode->set_fs_private(nullptr);
    }
    *inode = Inode{};
}

int64_t tmpfs_read(File* file, void* buf, size_t count, uint64_t* offset) noexcept {
    if (file == nullptr || file->get_inode() == nullptr || offset == nullptr) {
        return kEinval;
    }
    Inode* const inode = file->get_inode();
    if (inode->is_dir()) {
        return kEisdir;
    }
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kEinval;
    }

    if (*offset >= inode->get_size()) {
        return 0;
    }

    const auto* const node = static_cast<const TmpfsNode*>(inode->get_fs_private());
    if (node == nullptr) {
        return kEio;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uint64_t avail = inode->get_size() - *offset;
    const size_t to_read =
        (static_cast<uint64_t>(count) < avail) ? count : static_cast<size_t>(avail);

    auto* const dst = static_cast<uint8_t*>(buf);
    size_t total_read = 0U;

    while (total_read < to_read) {
        const auto page_idx = static_cast<size_t>(*offset / page_size);
        const auto page_off = static_cast<size_t>(*offset % page_size);
        const size_t page_rem = page_size - page_off;
        const size_t rem = to_read - total_read;
        const size_t chunk = (rem < page_rem) ? rem : page_rem;

        const uintptr_t frame = node->page_at(page_idx);
        if (frame == 0U) {
            std::memset(dst + total_read, 0, chunk);
        } else {
            const auto* const src =
                reinterpret_cast<const uint8_t*>(Kernel::MM::phys_to_virt(frame));
            std::memcpy(dst + total_read, src + page_off, chunk);
        }

        *offset += chunk;
        total_read += chunk;
    }

    inode->set_atime_ns(current_time_ns());
    return static_cast<int64_t>(total_read);
}

int64_t tmpfs_write(File* file, const void* buf, size_t count, uint64_t* offset) noexcept {
    if (file == nullptr || file->get_inode() == nullptr || offset == nullptr) {
        return kEinval;
    }
    Inode* const inode = file->get_inode();
    if (inode->is_dir()) {
        return kEisdir;
    }
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kEinval;
    }

    auto* const node = static_cast<TmpfsNode*>(inode->get_fs_private());
    if (node == nullptr) {
        return kEio;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const auto* const src = static_cast<const uint8_t*>(buf);
    size_t total_written = 0U;

    while (total_written < count) {
        const auto page_idx = static_cast<size_t>(*offset / page_size);
        if (page_idx >= kTmpfsMaxPagesPerFile) {
            if (total_written > 0U) {
                break;
            }
            return kEnospc;
        }

        const uintptr_t frame = node->ensure_page(page_idx, page_size);
        if (frame == 0U) {
            if (total_written > 0U) {
                break;
            }
            return kEnomem;
        }

        const auto page_off = static_cast<size_t>(*offset % page_size);
        const size_t page_rem = page_size - page_off;
        const size_t rem = count - total_written;
        const size_t chunk = (rem < page_rem) ? rem : page_rem;

        auto* const dst = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(frame));
        std::memcpy(dst + page_off, src + total_written, chunk);

        *offset += chunk;
        total_written += chunk;
    }

    if (*offset > inode->get_size()) {
        inode->set_size(*offset);
    }
    const uint64_t now_ns = current_time_ns();
    inode->set_mtime_ns(now_ns);
    inode->set_ctime_ns(now_ns);
    return static_cast<int64_t>(total_written);
}

int32_t tmpfs_readdir(File* file, DirEntryInfo* out_entry) noexcept {
    if (file == nullptr || file->get_dentry() == nullptr || out_entry == nullptr) {
        return kEinval;
    }
    const uint64_t target_idx = file->get_offset();
    uint64_t idx = 0U;
    for (Dentry* child = file->get_dentry()->get_first_child(); child != nullptr;
         child = child->get_next_sibling()) {
        Inode* const child_inode = child->get_inode();
        if (!child->is_active() || child_inode == nullptr) {
            continue;
        }
        if (idx == target_idx) {
            out_entry->ino = child_inode->get_ino();
            out_entry->type = child_inode->get_mode() & kModeTypeMask;
            const StringView child_name = child->name_view();
            for (size_t name_idx = 0U; name_idx <= kMaxNameLen; ++name_idx) {
                out_entry->name[name_idx] =
                    (name_idx < child_name.size()) ? child_name[name_idx] : '\0';
            }
            file->set_offset(target_idx + 1U);
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

    auto* const node = static_cast<TmpfsNode*>(inode->get_fs_private());
    if (node == nullptr) {
        return kEio;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uint64_t max_bytes = static_cast<uint64_t>(kTmpfsMaxPagesPerFile) * page_size;
    if (new_size > max_bytes) {
        return kEnospc;
    }

    if (new_size < inode->get_size()) {
        node->truncate_pages(new_size, page_size);
    }

    inode->set_size(new_size);
    const uint64_t now_ns = current_time_ns();
    inode->set_mtime_ns(now_ns);
    inode->set_ctime_ns(now_ns);
    return kOk;
}

extern const InodeOperations kTmpfsInodeOps;
extern const FileOperations kTmpfsFileOps;

int32_t tmpfs_create(Inode* dir, Dentry* target, uint32_t mode) noexcept {
    if (dir == nullptr || target == nullptr || dir->get_sb() == nullptr) {
        return kEinval;
    }

    TmpfsNode* const node = alloc_tmpfs_node();
    if (node == nullptr) {
        return kEnomem;
    }

    auto inode = RefPtr<Inode>::adopt(vfs_alloc_inode(dir->get_sb()));
    if (!inode) {
        node->reset();
        return kEnomem;
    }

    const uint64_t now_ns = current_time_ns();
    inode->set_ino(g_next_tmpfs_ino++);
    inode->set_mode(kModeReg | (mode & 07777U));
    inode->set_nlink(1U);
    inode->set_size(0U);
    inode->set_atime_ns(now_ns);
    inode->set_mtime_ns(now_ns);
    inode->set_ctime_ns(now_ns);
    inode->set_i_ops(&kTmpfsInodeOps);
    inode->set_f_ops(&kTmpfsFileOps);
    inode->set_fs_private(node);

    target->bind_inode(inode.detach());
    dir->set_mtime_ns(now_ns);
    dir->set_ctime_ns(now_ns);
    return kOk;
}

int32_t tmpfs_mkdir(Inode* dir, Dentry* target, uint32_t mode) noexcept {
    if (dir == nullptr || target == nullptr || dir->get_sb() == nullptr) {
        return kEinval;
    }

    auto inode = RefPtr<Inode>::adopt(vfs_alloc_inode(dir->get_sb()));
    if (!inode) {
        return kEnomem;
    }

    const uint64_t now_ns = current_time_ns();
    inode->set_ino(g_next_tmpfs_ino++);
    inode->set_mode(kModeDir | (mode & 07777U));
    inode->set_nlink(2U);
    inode->set_size(0U);
    inode->set_atime_ns(now_ns);
    inode->set_mtime_ns(now_ns);
    inode->set_ctime_ns(now_ns);
    inode->set_i_ops(&kTmpfsInodeOps);
    inode->set_f_ops(&kTmpfsFileOps);

    target->bind_inode(inode.detach());
    dir->set_nlink(dir->get_nlink() + 1U);
    dir->set_mtime_ns(now_ns);
    dir->set_ctime_ns(now_ns);
    return kOk;
}

int32_t tmpfs_unlink(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || target->get_inode() == nullptr) {
        return kEinval;
    }

    Inode* const target_inode = target->get_inode();
    const uint64_t now_ns = current_time_ns();
    if (target_inode->get_nlink() > 0U) {
        target_inode->set_nlink(target_inode->get_nlink() - 1U);
    }
    target_inode->set_ctime_ns(now_ns);
    dir->set_mtime_ns(now_ns);
    dir->set_ctime_ns(now_ns);
    return kOk;
}

int32_t tmpfs_rmdir(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || target->get_inode() == nullptr) {
        return kEinval;
    }

    Inode* const target_inode = target->get_inode();
    const uint64_t now_ns = current_time_ns();
    if (dir->get_nlink() > 0U) {
        dir->set_nlink(dir->get_nlink() - 1U);
    }
    target_inode->set_nlink(0U);
    target_inode->set_ctime_ns(now_ns);
    dir->set_mtime_ns(now_ns);
    dir->set_ctime_ns(now_ns);
    return kOk;
}

int32_t tmpfs_rename(Inode* old_dir, Dentry* old_dentry, Inode* new_dir,
                     Dentry* new_dentry) noexcept {
    if (old_dir == nullptr || old_dentry == nullptr || new_dir == nullptr) {
        return kEinval;
    }

    const uint64_t now_ns = current_time_ns();

    if (new_dentry != nullptr) {
        if (Inode* const dst_inode = new_dentry->get_inode(); dst_inode != nullptr) {
            if (dst_inode->is_dir() && new_dir->get_nlink() > 0U) {
                new_dir->set_nlink(new_dir->get_nlink() - 1U);
            }
            if (dst_inode->get_nlink() > 0U) {
                dst_inode->set_nlink(dst_inode->get_nlink() - 1U);
            }
            dst_inode->set_ctime_ns(now_ns);
        }
    }

    if (Inode* const src_inode = old_dentry->get_inode(); src_inode != nullptr) {
        if (src_inode->is_dir() && old_dir != new_dir) {
            if (old_dir->get_nlink() > 0U) {
                old_dir->set_nlink(old_dir->get_nlink() - 1U);
            }
            new_dir->set_nlink(new_dir->get_nlink() + 1U);
        }
        src_inode->set_ctime_ns(now_ns);
    }

    old_dir->set_mtime_ns(now_ns);
    old_dir->set_ctime_ns(now_ns);
    new_dir->set_mtime_ns(now_ns);
    new_dir->set_ctime_ns(now_ns);
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
    for (auto& slot : g_tmpfs_nodes) {
        slot.reset();
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
    superblock->set_fs_type("tmpfs");
    superblock->set_block_size(4096U);
    superblock->set_flags(0U);
    superblock->set_ops(&kTmpfsSbOps);

    auto root_inode = RefPtr<Inode>::adopt(vfs_alloc_inode(superblock));
    if (!root_inode) {
        vfs_free_superblock(superblock);
        return kEnomem;
    }

    const uint64_t now_ns = current_time_ns();
    root_inode->set_ino(1ULL);
    root_inode->set_mode(kModeDir | 01777U);
    root_inode->set_nlink(2U);
    root_inode->set_atime_ns(now_ns);
    root_inode->set_mtime_ns(now_ns);
    root_inode->set_ctime_ns(now_ns);
    root_inode->set_i_ops(&kTmpfsInodeOps);
    root_inode->set_f_ops(&kTmpfsFileOps);

    auto root_dentry = RefPtr<Dentry>::adopt(vfs_alloc_dentry(superblock, nullptr, "tmp"));
    if (!root_dentry) {
        vfs_free_superblock(superblock);
        return kEnomem;
    }
    root_dentry->bind_inode(root_inode.detach());
    superblock->set_root(root_dentry.detach());

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
        if (Dentry* const root = superblock->get_root(); root != nullptr) {
            root->release();
        }
        vfs_free_superblock(superblock);
        return mount_res;
    }

    g_tmpfs_sb = superblock;
    return kOk;
}

} // namespace Kernel::FS
