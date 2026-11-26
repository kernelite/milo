#include "ipc/Pipe.hpp"

#include "hal/cpu.hpp"
#include "process/Process.hpp"
#include "sync/IrqGuard.hpp"

#include <new>

namespace Kernel::IPC {

namespace {

constexpr int16_t kPollIn = 0x0001;
constexpr int16_t kPollOut = 0x0004;
constexpr int16_t kPollErr = 0x0008;
constexpr int16_t kPollHup = 0x0010;
constexpr int16_t kPollNval = 0x0020;
constexpr int16_t kPollRdNorm = 0x0040;
constexpr int16_t kPollWrNorm = 0x0100;

int64_t pipe_vfs_read(Kernel::FS::File* file, void* buf, size_t count,
                      uint64_t* /*offset*/) noexcept {
    if (file == nullptr || file->inode == nullptr) {
        return Kernel::FS::kEinval;
    }
    auto* const pipe = static_cast<Pipe*>(file->inode->fs_private);
    if (pipe == nullptr) {
        return Kernel::FS::kEinval;
    }
    const bool nonblock = (file->flags & Kernel::FS::kOpenNonBlock) != 0U;
    return pipe->read(buf, count, nonblock);
}

int64_t pipe_vfs_write(Kernel::FS::File* file, const void* buf, size_t count,
                       uint64_t* /*offset*/) noexcept {
    if (file == nullptr || file->inode == nullptr) {
        return Kernel::FS::kEinval;
    }
    auto* const pipe = static_cast<Pipe*>(file->inode->fs_private);
    if (pipe == nullptr) {
        return Kernel::FS::kEinval;
    }
    const bool nonblock = (file->flags & Kernel::FS::kOpenNonBlock) != 0U;
    return pipe->write(buf, count, nonblock);
}

int32_t pipe_vfs_open(Kernel::FS::Inode* inode, Kernel::FS::File* file) noexcept {
    if (inode == nullptr || file == nullptr) {
        return Kernel::FS::kEinval;
    }
    auto* const pipe = static_cast<Pipe*>(inode->fs_private);
    if (pipe == nullptr) {
        return Kernel::FS::kEinval;
    }
    const uint32_t acc = file->flags & Kernel::FS::kOpenAccessMask;
    if (acc == Kernel::FS::kOpenReadOnly || acc == Kernel::FS::kOpenReadWrite) {
        pipe->retain_reader();
    }
    if (acc == Kernel::FS::kOpenWriteOnly || acc == Kernel::FS::kOpenReadWrite) {
        pipe->retain_writer();
    }
    return Kernel::FS::kOk;
}

int32_t pipe_vfs_release(Kernel::FS::Inode* inode, Kernel::FS::File* file) noexcept {
    if (inode == nullptr || file == nullptr) {
        return Kernel::FS::kEinval;
    }
    auto* const pipe = static_cast<Pipe*>(inode->fs_private);
    if (pipe == nullptr) {
        return Kernel::FS::kEinval;
    }
    const uint32_t acc = file->flags & Kernel::FS::kOpenAccessMask;
    const bool is_reader = (acc == Kernel::FS::kOpenReadOnly || acc == Kernel::FS::kOpenReadWrite);
    const bool is_writer = (acc == Kernel::FS::kOpenWriteOnly || acc == Kernel::FS::kOpenReadWrite);
    pipe->release_end(is_reader, is_writer);
    return Kernel::FS::kOk;
}

int64_t pipe_vfs_lseek(Kernel::FS::File* /*file*/, int64_t /*offset*/,
                       Kernel::FS::SeekWhence /*whence*/) noexcept {
    return Kernel::FS::kEspipe;
}

int16_t pipe_vfs_poll(Kernel::FS::File* file, int16_t events) noexcept {
    if (file == nullptr || file->inode == nullptr) {
        return kPollNval;
    }
    const auto* const pipe = static_cast<const Pipe*>(file->inode->fs_private);
    if (pipe == nullptr) {
        return kPollNval;
    }
    const uint32_t acc = file->flags & Kernel::FS::kOpenAccessMask;
    return pipe->poll(acc, events);
}

const Kernel::FS::FileOperations kPipeFileOps{
    .read = pipe_vfs_read,
    .write = pipe_vfs_write,
    .open = pipe_vfs_open,
    .release = pipe_vfs_release,
    .lseek = pipe_vfs_lseek,
    .ioctl = nullptr,
    .fsync = nullptr,
    .readdir = nullptr,
    .poll = pipe_vfs_poll,
};

} // namespace

void Pipe::retain_reader() noexcept {
    const Sync::IrqGuard irq_guard;
    if (m_in_use) {
        ++m_readers;
    }
}

void Pipe::retain_writer() noexcept {
    const Sync::IrqGuard irq_guard;
    if (m_in_use) {
        ++m_writers;
    }
}

void Pipe::release_end(bool release_reader, bool release_writer) noexcept {
    if (!release_reader && !release_writer) {
        return;
    }

    bool should_delete = false;
    {
        const Sync::IrqGuard irq_guard;
        if (!m_in_use) {
            return;
        }

        if (release_reader) {
            if (m_readers > 0U) {
                --m_readers;
            }
            m_write_wq.wake_all();
        }
        if (release_writer) {
            if (m_writers > 0U) {
                --m_writers;
            }
            m_read_wq.wake_all();
        }

        should_delete = (m_readers == 0U && m_writers == 0U);
        if (should_delete) {
            m_in_use = false;
        }
    }

    if (should_delete) {
        delete this;
    }
}

int64_t Pipe::read(void* dst, size_t count, bool nonblock) noexcept {
    if (!m_in_use) {
        return Kernel::FS::kEinval;
    }
    if (count == 0U) {
        return 0;
    }
    if (dst == nullptr) {
        return Kernel::FS::kEinval;
    }

    auto& cpu = HAL::get_cpu();
    const Sync::IrqGuard irq_guard;

    while (m_count == 0U) {
        if (m_writers == 0U) {
            return 0; // EOF
        }
        if (nonblock) {
            return Kernel::FS::kEagain;
        }
        const auto* const cur = Kernel::Process::Process::current();
        if (cur != nullptr && Kernel::Process::has_unblocked_pending_signal(*cur)) {
            return Kernel::FS::kEintr;
        }
        m_read_wq.sleep();
        cpu.disable_interrupts();
    }

    const size_t to_read = (count < m_count) ? count : m_count;
    auto* const out = static_cast<uint8_t*>(dst);
    for (size_t idx = 0U; idx < to_read; ++idx) {
        out[idx] = m_buffer[m_read_pos];
        m_read_pos = (m_read_pos + 1U) % PIPE_BUF_SIZE;
    }
    m_count -= to_read;

    m_write_wq.wake_all();
    return static_cast<int64_t>(to_read);
}

int64_t Pipe::write(const void* src, size_t count, bool nonblock) noexcept {
    if (!m_in_use) {
        return Kernel::FS::kEinval;
    }
    if (count == 0U) {
        return 0;
    }
    if (src == nullptr) {
        return Kernel::FS::kEinval;
    }

    auto& cpu = HAL::get_cpu();
    const Sync::IrqGuard irq_guard;

    const auto* const in_bytes = static_cast<const uint8_t*>(src);
    size_t total_written = 0U;

    while (total_written < count) {
        if (m_readers == 0U) {
            m_read_wq.wake_all();
            if (auto* const cur = Kernel::Process::Process::current(); cur != nullptr) {
                Kernel::Process::send_signal(*cur, Kernel::Process::SIG_PIPE);
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : Kernel::FS::kEpipe;
        }

        while (m_count == PIPE_BUF_SIZE) {
            if (m_readers == 0U) {
                if (auto* const cur = Kernel::Process::Process::current(); cur != nullptr) {
                    Kernel::Process::send_signal(*cur, Kernel::Process::SIG_PIPE);
                }
                return (total_written > 0U) ? static_cast<int64_t>(total_written)
                                            : Kernel::FS::kEpipe;
            }
            if (nonblock) {
                m_read_wq.wake_all();
                return (total_written > 0U) ? static_cast<int64_t>(total_written)
                                            : Kernel::FS::kEagain;
            }
            const auto* const cur = Kernel::Process::Process::current();
            if (cur != nullptr && Kernel::Process::has_unblocked_pending_signal(*cur)) {
                return (total_written > 0U) ? static_cast<int64_t>(total_written)
                                            : Kernel::FS::kEintr;
            }
            m_write_wq.sleep();
            cpu.disable_interrupts();
        }

        const size_t space = PIPE_BUF_SIZE - m_count;
        const size_t rem = count - total_written;
        const size_t batch = (rem < space) ? rem : space;

        for (size_t idx = 0U; idx < batch; ++idx) {
            m_buffer[m_write_pos] = in_bytes[total_written + idx];
            m_write_pos = (m_write_pos + 1U) % PIPE_BUF_SIZE;
        }
        m_count += batch;
        total_written += batch;
        m_read_wq.wake_all();
    }

    return static_cast<int64_t>(total_written);
}

int16_t Pipe::poll(uint32_t acc_mode, int16_t events) const noexcept {
    if (!m_in_use) {
        return kPollNval;
    }

    const auto event_mask = static_cast<uint16_t>(events);
    constexpr auto kInMask =
        static_cast<uint16_t>(static_cast<uint16_t>(kPollIn) | static_cast<uint16_t>(kPollRdNorm));
    constexpr auto kOutMask =
        static_cast<uint16_t>(static_cast<uint16_t>(kPollOut) | static_cast<uint16_t>(kPollWrNorm));

    uint16_t revents = 0U;
    if (acc_mode == Kernel::FS::kOpenReadOnly) {
        if (m_count > 0U) {
            revents |= static_cast<uint16_t>(event_mask & kInMask);
        }
        if (m_writers == 0U) {
            revents |= static_cast<uint16_t>(kPollHup);
        }
    } else if (acc_mode == Kernel::FS::kOpenWriteOnly) {
        if (m_readers == 0U) {
            revents |= static_cast<uint16_t>(kPollErr);
        } else if (m_count < PIPE_BUF_SIZE) {
            revents |= static_cast<uint16_t>(event_mask & kOutMask);
        }
    }
    return static_cast<int16_t>(revents);
}

int32_t Pipe::create_files(Kernel::FS::File** out_read_file, Kernel::FS::File** out_write_file,
                           uint32_t extra_flags) noexcept {
    if (out_read_file == nullptr || out_write_file == nullptr) {
        return Kernel::FS::kEinval;
    }
    *out_read_file = nullptr;
    *out_write_file = nullptr;

    auto* const pipe = new (std::nothrow) Pipe{};
    if (pipe == nullptr) {
        return Kernel::FS::kEnomem;
    }
    pipe->m_in_use = true;
    pipe->m_readers = 1U;
    pipe->m_writers = 1U;

    Kernel::FS::Inode* inode = Kernel::FS::vfs_alloc_inode(nullptr);
    if (inode == nullptr) {
        delete pipe;
        return Kernel::FS::kEnomem;
    }
    inode->ino = 3ULL;
    inode->mode = Kernel::FS::kModeFifo | 0600U;
    inode->fs_private = pipe;
    inode->f_ops = &kPipeFileOps;

    Kernel::FS::File* read_file = Kernel::FS::vfs_alloc_file();
    if (read_file == nullptr) {
        Kernel::FS::inode_put(inode);
        delete pipe;
        return Kernel::FS::kEnfile;
    }
    Kernel::FS::inode_get(inode);
    read_file->inode = inode;
    read_file->flags = Kernel::FS::kOpenReadOnly | extra_flags;
    read_file->f_ops = &kPipeFileOps;

    Kernel::FS::File* write_file = Kernel::FS::vfs_alloc_file();
    if (write_file == nullptr) {
        pipe->m_writers = 0U;
        Kernel::FS::file_put(read_file);
        Kernel::FS::inode_put(inode);
        return Kernel::FS::kEnfile;
    }
    Kernel::FS::inode_get(inode);
    write_file->inode = inode;
    write_file->flags = Kernel::FS::kOpenWriteOnly | extra_flags;
    write_file->f_ops = &kPipeFileOps;

    Kernel::FS::inode_put(inode);

    *out_read_file = read_file;
    *out_write_file = write_file;
    return Kernel::FS::kOk;
}

} // namespace Kernel::IPC
