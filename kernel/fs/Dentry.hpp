#pragma once

#include "fs/VfsTypes.hpp"

namespace Kernel::FS {

struct Dentry {
    char name[kMaxNameLen + 1U]{};
    uint8_t name_len{0U};
    Inode* inode{nullptr};
    Superblock* sb{nullptr};
    Dentry* parent{nullptr};
    Dentry* first_child{nullptr};
    Dentry* next_sibling{nullptr};
    uint32_t refcount{0U};
    uint32_t flags{0U};
    bool active{false};

    void retain() noexcept;
    void release() noexcept;

    void attach_child(Dentry* child) noexcept;
    void detach() noexcept;
    [[nodiscard]] Dentry* find_child(StringView child_name) const noexcept;
    [[nodiscard]] constexpr bool has_children() const noexcept { return first_child != nullptr; }

    [[nodiscard]] constexpr Dentry* get_first_child() const noexcept { return first_child; }
    [[nodiscard]] constexpr Dentry* get_next_sibling() const noexcept { return next_sibling; }
    [[nodiscard]] constexpr Dentry* get_parent() const noexcept { return parent; }
    constexpr void set_parent(Dentry* new_parent) noexcept { parent = new_parent; }

    [[nodiscard]] constexpr Inode* get_inode() const noexcept { return inode; }
    constexpr void bind_inode(Inode* new_inode) noexcept { inode = new_inode; }

    [[nodiscard]] constexpr Superblock* get_sb() const noexcept { return sb; }
    constexpr void set_sb(Superblock* new_sb) noexcept { sb = new_sb; }

    [[nodiscard]] constexpr uint32_t get_refcount() const noexcept { return refcount; }
    [[nodiscard]] constexpr bool is_active() const noexcept { return active; }

    void set_name(StringView new_name) noexcept {
        const size_t copy_len = (new_name.size() < kMaxNameLen) ? new_name.size() : kMaxNameLen;
        for (size_t idx = 0U; idx < copy_len; ++idx) {
            name[idx] = new_name[idx];
        }
        name[copy_len] = '\0';
        name_len = static_cast<uint8_t>(copy_len);
    }

    constexpr void mark_root() noexcept {
        flags |= kDentryRoot;
        parent = nullptr;
    }

    constexpr void set_mounted(bool mounted) noexcept {
        if (mounted) {
            flags |= kDentryMounted;
        } else {
            flags &= ~kDentryMounted;
        }
    }

    [[nodiscard]] constexpr StringView name_view() const noexcept { return {name, name_len}; }
    [[nodiscard]] constexpr bool is_mounted() const noexcept {
        return (flags & kDentryMounted) != 0U;
    }
    [[nodiscard]] constexpr bool is_root() const noexcept { return (flags & kDentryRoot) != 0U; }
};

} // namespace Kernel::FS
