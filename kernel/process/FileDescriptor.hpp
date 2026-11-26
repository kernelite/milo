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

class FileDescriptor {
  public:
    constexpr FileDescriptor() noexcept = default;
    constexpr FileDescriptor(Kernel::FS::File* file, uint32_t fd_flags) noexcept
        : m_file(file), m_fd_flags(fd_flags) {}

    [[nodiscard]] constexpr Kernel::FS::File* file() const noexcept { return m_file; }
    [[nodiscard]] constexpr uint32_t flags() const noexcept { return m_fd_flags; }

    [[nodiscard]] constexpr bool has_cloexec() const noexcept {
        return (m_fd_flags & FD_FLAG_CLOEXEC) != 0U;
    }

    constexpr void set_cloexec(bool enable) noexcept {
        if (enable) {
            m_fd_flags |= FD_FLAG_CLOEXEC;
        } else {
            m_fd_flags &= ~FD_FLAG_CLOEXEC;
        }
    }

    constexpr void reset() noexcept {
        m_file = nullptr;
        m_fd_flags = 0U;
    }

    [[nodiscard]] bool is_open() const noexcept { return m_file != nullptr && m_file->is_active(); }

    [[nodiscard]] bool is_readable() const noexcept {
        if (!is_open()) {
            return false;
        }
        const uint32_t acc = m_file->get_flags() & Kernel::FS::kOpenAccessMask;
        return acc == Kernel::FS::kOpenReadOnly || acc == Kernel::FS::kOpenReadWrite;
    }

    [[nodiscard]] bool is_writable() const noexcept {
        if (!is_open()) {
            return false;
        }
        const uint32_t acc = m_file->get_flags() & Kernel::FS::kOpenAccessMask;
        return acc == Kernel::FS::kOpenWriteOnly || acc == Kernel::FS::kOpenReadWrite;
    }

    [[nodiscard]] bool is_tty() const noexcept {
        return is_open() && m_file->get_inode() != nullptr && m_file->get_inode()->is_chr() &&
               m_file->get_f_ops() != nullptr && m_file->get_f_ops()->ioctl != nullptr;
    }

  private:
    Kernel::FS::File* m_file{nullptr};
    uint32_t m_fd_flags{0U};
};

} // namespace Kernel::Process
