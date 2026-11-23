#include "ipc/pipe.hpp"

#include "hal/cpu.hpp"
#include "process/process.hpp"

#include <new>

namespace {

constexpr int16_t kPollIn = 0x0001;
constexpr int16_t kPollOut = 0x0004;
constexpr int16_t kPollErr = 0x0008;
constexpr int16_t kPollHup = 0x0010;
constexpr int16_t kPollNval = 0x0020;
constexpr int16_t kPollRdNorm = 0x0040;
constexpr int16_t kPollWrNorm = 0x0100;

using Kernel::IPC::Pipe;
using Kernel::IPC::PIPE_BUF_SIZE;

void retain_pipe_reader(Pipe* pipe) noexcept {
    if (pipe == nullptr) {
        return;
    }
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    if (pipe->in_use) {
        ++pipe->readers;
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void retain_pipe_writer(Pipe* pipe) noexcept {
    if (pipe == nullptr) {
        return;
    }
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    if (pipe->in_use) {
        ++pipe->writers;
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void release_pipe_end(Pipe* pipe, bool release_reader, bool release_writer) noexcept {
    if (pipe == nullptr || (!release_reader && !release_writer)) {
        return;
    }
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    if (!pipe->in_use) {
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
        return;
    }

    if (release_reader) {
        if (pipe->readers > 0U) {
            --pipe->readers;
        }
        pipe->write_wq.wake_all();
    }
    if (release_writer) {
        if (pipe->writers > 0U) {
            --pipe->writers;
        }
        pipe->read_wq.wake_all();
    }

    const bool should_delete = (pipe->readers == 0U && pipe->writers == 0U);
    if (should_delete) {
        pipe->in_use = false;
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }

    if (should_delete) {
        delete pipe;
    }
}

int64_t pipe_vfs_read(kernel::fs::File* file, void* buf, size_t count,
                      uint64_t* /*offset*/) noexcept {
    if (file == nullptr || file->inode == nullptr) {
        return kernel::fs::kEinval;
    }
    auto* const pipe = static_cast<Pipe*>(file->inode->fs_private);
    if (pipe == nullptr || !pipe->in_use) {
        return kernel::fs::kEinval;
    }
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kernel::fs::kEinval;
    }

    const bool nonblock = (file->flags & kernel::fs::kOpenNonBlock) != 0U;
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    while (pipe->count == 0U) {
        if (pipe->writers == 0U) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return 0; // EOF
        }
        if (nonblock) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return kernel::fs::kEagain;
        }
        const auto* const cur = Kernel::Process::current_process();
        if (cur != nullptr && Kernel::Process::has_unblocked_pending_signal(*cur)) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return kernel::fs::kEintr;
        }
        pipe->read_wq.sleep();
        cpu.disable_interrupts();
    }

    const size_t to_read = (count < pipe->count) ? count : pipe->count;
    auto* const dst = static_cast<uint8_t*>(buf);
    for (size_t idx = 0U; idx < to_read; ++idx) {
        dst[idx] = pipe->buffer[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1U) % PIPE_BUF_SIZE;
    }
    pipe->count -= to_read;

    pipe->write_wq.wake_all();
    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
    return static_cast<int64_t>(to_read);
}

int64_t pipe_vfs_write(kernel::fs::File* file, const void* buf, size_t count,
                       uint64_t* /*offset*/) noexcept {
    if (file == nullptr || file->inode == nullptr) {
        return kernel::fs::kEinval;
    }
    auto* const pipe = static_cast<Pipe*>(file->inode->fs_private);
    if (pipe == nullptr || !pipe->in_use) {
        return kernel::fs::kEinval;
    }
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kernel::fs::kEinval;
    }

    const bool nonblock = (file->flags & kernel::fs::kOpenNonBlock) != 0U;
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    const auto* const src = static_cast<const uint8_t*>(buf);
    size_t total_written = 0U;

    while (total_written < count) {
        if (pipe->readers == 0U) {
            pipe->read_wq.wake_all();
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            auto* const cur = Kernel::Process::current_process();
            if (cur != nullptr) {
                Kernel::Process::send_signal(*cur, Kernel::Process::SIG_PIPE);
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : kernel::fs::kEpipe;
        }

        while (pipe->count == PIPE_BUF_SIZE) {
            if (pipe->readers == 0U) {
                if (irq_was_enabled) {
                    cpu.enable_interrupts();
                }
                auto* const cur = Kernel::Process::current_process();
                if (cur != nullptr) {
                    Kernel::Process::send_signal(*cur, Kernel::Process::SIG_PIPE);
                }
                return (total_written > 0U) ? static_cast<int64_t>(total_written)
                                            : kernel::fs::kEpipe;
            }
            if (nonblock) {
                pipe->read_wq.wake_all();
                if (irq_was_enabled) {
                    cpu.enable_interrupts();
                }
                return (total_written > 0U) ? static_cast<int64_t>(total_written)
                                            : kernel::fs::kEagain;
            }
            const auto* const cur = Kernel::Process::current_process();
            if (cur != nullptr && Kernel::Process::has_unblocked_pending_signal(*cur)) {
                if (irq_was_enabled) {
                    cpu.enable_interrupts();
                }
                return (total_written > 0U) ? static_cast<int64_t>(total_written)
                                            : kernel::fs::kEintr;
            }
            pipe->write_wq.sleep();
            cpu.disable_interrupts();
        }

        const size_t space = PIPE_BUF_SIZE - pipe->count;
        const size_t rem = count - total_written;
        const size_t batch = (rem < space) ? rem : space;

        for (size_t idx = 0U; idx < batch; ++idx) {
            pipe->buffer[pipe->write_pos] = src[total_written + idx];
            pipe->write_pos = (pipe->write_pos + 1U) % PIPE_BUF_SIZE;
        }
        pipe->count += batch;
        total_written += batch;
        pipe->read_wq.wake_all();
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
    return static_cast<int64_t>(total_written);
}

int32_t pipe_vfs_open(kernel::fs::Inode* inode, kernel::fs::File* file) noexcept {
    if (inode == nullptr || file == nullptr) {
        return kernel::fs::kEinval;
    }
    auto* const pipe = static_cast<Pipe*>(inode->fs_private);
    const uint32_t acc = file->flags & kernel::fs::kOpenAccessMask;
    if (acc == kernel::fs::kOpenReadOnly || acc == kernel::fs::kOpenReadWrite) {
        retain_pipe_reader(pipe);
    }
    if (acc == kernel::fs::kOpenWriteOnly || acc == kernel::fs::kOpenReadWrite) {
        retain_pipe_writer(pipe);
    }
    return kernel::fs::kOk;
}

int32_t pipe_vfs_release(kernel::fs::Inode* inode, kernel::fs::File* file) noexcept {
    if (inode == nullptr || file == nullptr) {
        return kernel::fs::kEinval;
    }
    auto* const pipe = static_cast<Pipe*>(inode->fs_private);
    const uint32_t acc = file->flags & kernel::fs::kOpenAccessMask;
    const bool is_reader = (acc == kernel::fs::kOpenReadOnly || acc == kernel::fs::kOpenReadWrite);
    const bool is_writer = (acc == kernel::fs::kOpenWriteOnly || acc == kernel::fs::kOpenReadWrite);
    release_pipe_end(pipe, is_reader, is_writer);
    return kernel::fs::kOk;
}

int64_t pipe_vfs_lseek(kernel::fs::File* /*file*/, int64_t /*offset*/,
                       kernel::fs::SeekWhence /*whence*/) noexcept {
    return kernel::fs::kEspipe;
}

int16_t pipe_vfs_poll(kernel::fs::File* file, int16_t events) noexcept {
    if (file == nullptr || file->inode == nullptr) {
        return kPollNval;
    }
    const auto* const pipe = static_cast<const Pipe*>(file->inode->fs_private);
    if (pipe == nullptr || !pipe->in_use) {
        return kPollNval;
    }

    const uint32_t acc = file->flags & kernel::fs::kOpenAccessMask;
    uint16_t revents = 0U;
    if (acc == kernel::fs::kOpenReadOnly) {
        if (pipe->count > 0U) {
            revents |= static_cast<uint16_t>(events & (kPollIn | kPollRdNorm));
        }
        if (pipe->writers == 0U) {
            revents |= static_cast<uint16_t>(kPollHup);
        }
    } else if (acc == kernel::fs::kOpenWriteOnly) {
        if (pipe->readers == 0U) {
            revents |= static_cast<uint16_t>(kPollErr);
        } else if (pipe->count < PIPE_BUF_SIZE) {
            revents |= static_cast<uint16_t>(events & (kPollOut | kPollWrNorm));
        }
    }
    return static_cast<int16_t>(revents);
}

const kernel::fs::FileOperations kPipeFileOps{
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

namespace Kernel::IPC {

int32_t create_pipe_files(kernel::fs::File** out_read_file, kernel::fs::File** out_write_file,
                          uint32_t extra_flags) noexcept {
    if (out_read_file == nullptr || out_write_file == nullptr) {
        return kernel::fs::kEinval;
    }
    *out_read_file = nullptr;
    *out_write_file = nullptr;

    auto* const pipe = new (std::nothrow) Pipe{};
    if (pipe == nullptr) {
        return kernel::fs::kEnomem;
    }
    pipe->in_use = true;
    pipe->readers = 1U;
    pipe->writers = 1U;

    kernel::fs::Inode* inode = kernel::fs::vfs_alloc_inode(nullptr);
    if (inode == nullptr) {
        delete pipe;
        return kernel::fs::kEnomem;
    }
    inode->ino = 3ULL;
    inode->mode = kernel::fs::kModeFifo | 0600U;
    inode->fs_private = pipe;
    inode->f_ops = &kPipeFileOps;

    kernel::fs::File* read_file = kernel::fs::vfs_alloc_file();
    if (read_file == nullptr) {
        kernel::fs::inode_put(inode);
        delete pipe;
        return kernel::fs::kEnfile;
    }
    kernel::fs::inode_get(inode);
    read_file->inode = inode;
    read_file->flags = kernel::fs::kOpenReadOnly | extra_flags;
    read_file->f_ops = &kPipeFileOps;

    kernel::fs::File* write_file = kernel::fs::vfs_alloc_file();
    if (write_file == nullptr) {
        pipe->writers = 0U;
        kernel::fs::file_put(read_file);
        kernel::fs::inode_put(inode);
        return kernel::fs::kEnfile;
    }
    kernel::fs::inode_get(inode);
    write_file->inode = inode;
    write_file->flags = kernel::fs::kOpenWriteOnly | extra_flags;
    write_file->f_ops = &kPipeFileOps;

    kernel::fs::inode_put(inode);

    *out_read_file = read_file;
    *out_write_file = write_file;
    return kernel::fs::kOk;
}

} // namespace Kernel::IPC