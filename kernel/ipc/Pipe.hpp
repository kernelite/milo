#pragma once

#include "fs/Vfs.hpp"
#include "process/wait_queue.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::IPC {

constexpr size_t PIPE_BUF_SIZE = 4096U;

class Pipe final {
  public:
    constexpr Pipe() noexcept = default;
    ~Pipe() noexcept = default;

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&&) = delete;
    Pipe& operator=(Pipe&&) = delete;

    // Allocates a Pipe ring buffer backed by an anonymous VFS Inode and read/write File instances.
    [[nodiscard]] static int32_t create_files(Kernel::FS::File** out_read_file,
                                              Kernel::FS::File** out_write_file,
                                              uint32_t extra_flags = 0U) noexcept;

    [[nodiscard]] int64_t read(void* dst, size_t count, bool nonblock) noexcept;
    [[nodiscard]] int64_t write(const void* src, size_t count, bool nonblock) noexcept;
    [[nodiscard]] int16_t poll(uint32_t acc_mode, int16_t events) const noexcept;

    void retain_reader() noexcept;
    void retain_writer() noexcept;
    void release_end(bool release_reader, bool release_writer) noexcept;

    [[nodiscard]] bool is_in_use() const noexcept { return m_in_use; }

  private:
    bool m_in_use{false};
    uint32_t m_readers{0U};
    uint32_t m_writers{0U};
    size_t m_read_pos{0U};
    size_t m_write_pos{0U};
    size_t m_count{0U};
    Process::WaitQueue m_read_wq;
    Process::WaitQueue m_write_wq;
    uint8_t m_buffer[PIPE_BUF_SIZE]{};
};

[[nodiscard]] inline int32_t create_pipe_files(Kernel::FS::File** out_read_file,
                                               Kernel::FS::File** out_write_file,
                                               uint32_t extra_flags = 0U) noexcept {
    return Pipe::create_files(out_read_file, out_write_file, extra_flags);
}

} // namespace Kernel::IPC
