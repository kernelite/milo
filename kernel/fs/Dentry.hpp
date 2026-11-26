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

    [[nodiscard]] StringView name_view() const noexcept { return {name, name_len}; }
    [[nodiscard]] bool is_mounted() const noexcept { return (flags & kDentryMounted) != 0U; }
    [[nodiscard]] bool is_root() const noexcept { return (flags & kDentryRoot) != 0U; }
};

} // namespace Kernel::FS
