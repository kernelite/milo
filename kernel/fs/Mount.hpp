#pragma once

#include "fs/VfsTypes.hpp"

namespace Kernel::FS {

struct Mount {
    Dentry* mountpoint{nullptr};
    Superblock* sb{nullptr};
    Mount* parent_mount{nullptr};
    bool active{false};
};

} // namespace Kernel::FS
