#pragma once

#include "fs/Vfs.hpp"
#include "process/wait_queue.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::IPC {

constexpr size_t PIPE_BUF_SIZE = 4096U;

struct Pipe {
    bool in_use{false};
    uint32_t readers{0U};
    uint32_t writers{0U};
    size_t read_pos{0U};
    size_t write_pos{0U};
    size_t count{0U};
    Process::WaitQueue read_wq;
    Process::WaitQueue write_wq;
    uint8_t buffer[PIPE_BUF_SIZE]{};
};

// Allocates a Pipe ring buffer backed by an anonymous VFS Inode and read/write File instances.
[[nodiscard]] int32_t create_pipe_files(Kernel::FS::File** out_read_file,
                                        Kernel::FS::File** out_write_file,
                                        uint32_t extra_flags = 0U) noexcept;

} // namespace Kernel::IPC