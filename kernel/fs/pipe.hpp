#pragma once

#include "process/wait_queue.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::FS {

constexpr size_t MAX_PIPES = 64U;
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

Pipe* allocate_pipe() noexcept;
void retain_pipe_reader(Pipe* pipe) noexcept;
void retain_pipe_writer(Pipe* pipe) noexcept;
void release_pipe_reader(Pipe* pipe) noexcept;
void release_pipe_writer(Pipe* pipe) noexcept;

int64_t pipe_read(Pipe* pipe, uintptr_t root_paddr, uintptr_t user_buf, size_t count,
                  bool nonblock) noexcept;
int64_t pipe_write(Pipe* pipe, uintptr_t root_paddr, uintptr_t user_buf, size_t count,
                   bool nonblock) noexcept;

} // namespace Kernel::FS