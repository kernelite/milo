#pragma once

#include "fs/VfsTypes.hpp"

namespace Kernel::FS {

class Dentry final {
  public:
    constexpr Dentry() noexcept = default;
    ~Dentry() noexcept = default;

    Dentry(const Dentry&) = delete;
    Dentry& operator=(const Dentry&) = delete;
    Dentry(Dentry&&) = delete;
    Dentry& operator=(Dentry&&) = delete;

    void reset() noexcept {
        for (char& ch : m_name) {
            ch = '\0';
        }
        m_name_len = 0U;
        m_inode = nullptr;
        m_sb = nullptr;
        m_parent = nullptr;
        m_first_child = nullptr;
        m_next_sibling = nullptr;
        m_refcount = 0U;
        m_flags = 0U;
        m_active = false;
    }

    void activate(Superblock* sb_ptr, Dentry* parent_ptr, StringView dentry_name) noexcept {
        reset();
        m_sb = sb_ptr;
        m_parent = parent_ptr;
        set_name(dentry_name);
        m_refcount = 1U;
        m_active = true;
    }

    void retain() noexcept;
    void release() noexcept;

    void attach_child(Dentry* child) noexcept;
    void detach() noexcept;
    [[nodiscard]] Dentry* find_child(StringView child_name) const noexcept;
    [[nodiscard]] constexpr bool has_children() const noexcept { return m_first_child != nullptr; }

    [[nodiscard]] constexpr Dentry* get_first_child() const noexcept { return m_first_child; }
    constexpr void set_first_child(Dentry* child) noexcept { m_first_child = child; }

    [[nodiscard]] constexpr Dentry* get_next_sibling() const noexcept { return m_next_sibling; }
    constexpr void set_next_sibling(Dentry* sibling) noexcept { m_next_sibling = sibling; }

    [[nodiscard]] constexpr Dentry* get_parent() const noexcept { return m_parent; }
    constexpr void set_parent(Dentry* new_parent) noexcept { m_parent = new_parent; }

    [[nodiscard]] constexpr Inode* get_inode() const noexcept { return m_inode; }
    constexpr void bind_inode(Inode* new_inode) noexcept { m_inode = new_inode; }

    [[nodiscard]] constexpr Superblock* get_sb() const noexcept { return m_sb; }
    constexpr void set_sb(Superblock* new_sb) noexcept { m_sb = new_sb; }

    [[nodiscard]] constexpr uint32_t get_refcount() const noexcept { return m_refcount; }
    constexpr void set_refcount(uint32_t new_refcount) noexcept { m_refcount = new_refcount; }

    [[nodiscard]] constexpr uint32_t get_flags() const noexcept { return m_flags; }
    constexpr void set_flags(uint32_t new_flags) noexcept { m_flags = new_flags; }

    [[nodiscard]] constexpr bool is_active() const noexcept { return m_active; }
    constexpr void set_active(bool active) noexcept { m_active = active; }

    void set_name(StringView new_name) noexcept {
        const size_t copy_len = (new_name.size() < kMaxNameLen) ? new_name.size() : kMaxNameLen;
        for (size_t idx = 0U; idx < copy_len; ++idx) {
            m_name[idx] = new_name[idx];
        }
        m_name[copy_len] = '\0';
        m_name_len = static_cast<uint8_t>(copy_len);
    }

    constexpr void mark_root() noexcept {
        m_flags |= kDentryRoot;
        m_parent = nullptr;
    }

    constexpr void set_mounted(bool mounted) noexcept {
        if (mounted) {
            m_flags |= kDentryMounted;
        } else {
            m_flags &= ~kDentryMounted;
        }
    }

    [[nodiscard]] constexpr StringView name_view() const noexcept { return {m_name, m_name_len}; }
    [[nodiscard]] constexpr bool is_mounted() const noexcept {
        return (m_flags & kDentryMounted) != 0U;
    }
    [[nodiscard]] constexpr bool is_root() const noexcept { return (m_flags & kDentryRoot) != 0U; }

  private:
    char m_name[kMaxNameLen + 1U]{};
    uint8_t m_name_len{0U};
    Inode* m_inode{nullptr};
    Superblock* m_sb{nullptr};
    Dentry* m_parent{nullptr};
    Dentry* m_first_child{nullptr};
    Dentry* m_next_sibling{nullptr};
    uint32_t m_refcount{0U};
    uint32_t m_flags{0U};
    bool m_active{false};
};

} // namespace Kernel::FS
