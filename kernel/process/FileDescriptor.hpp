#pragma once

#include "fs/File.hpp"
#include "fs/Inode.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Process {

constexpr size_t INITIAL_FD_CAPACITY = 16U;

constexpr uint32_t FD_FLAG_READ = 1U << 0U;
constexpr uint32_t FD_FLAG_WRITE = 1U << 1U;
constexpr uint32_t FD_FLAG_NONBLOCK = 1U << 2U;
constexpr uint32_t FD_FLAG_CLOEXEC = 1U << 3U;

struct FileDescriptor {
    Kernel::FS::File* file{nullptr};
    uint32_t fd_flags{0U};

    [[nodiscard]] bool is_open() const noexcept { return file != nullptr && file->active; }

    [[nodiscard]] bool is_readable() const noexcept {
        if (!is_open()) {
            return false;
        }
        const uint32_t acc = file->flags & Kernel::FS::kOpenAccessMask;
        return acc == Kernel::FS::kOpenReadOnly || acc == Kernel::FS::kOpenReadWrite;
    }

    [[nodiscard]] bool is_writable() const noexcept {
        if (!is_open()) {
            return false;
        }
        const uint32_t acc = file->flags & Kernel::FS::kOpenAccessMask;
        return acc == Kernel::FS::kOpenWriteOnly || acc == Kernel::FS::kOpenReadWrite;
    }

    [[nodiscard]] bool is_tty() const noexcept {
        return is_open() && file->inode != nullptr && file->inode->is_chr() &&
               file->f_ops != nullptr && file->f_ops->ioctl != nullptr;
    }
};

struct Process;

// Dynamic file descriptor table lifecycle & allocation
bool init_fd_table(Process& proc, size_t initial_capacity = INITIAL_FD_CAPACITY) noexcept;
void destroy_fd_table(Process& proc) noexcept;
bool ensure_fd_capacity(Process& proc, size_t required_slots) noexcept;
int allocate_fd(Process& proc, size_t min_fd = 0U) noexcept;
void close_fd(Process& proc, size_t fd_idx) noexcept;
void copy_fd_slot(FileDescriptor& dst, const FileDescriptor& src, bool clear_cloexec) noexcept;
void close_cloexec_fds(Process& proc) noexcept;

} // namespace Kernel::Process
