#pragma once

#include "process/FileDescriptor.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Process {

class FdTable final {
  public:
    constexpr FdTable() noexcept = default;
    ~FdTable() noexcept { destroy(); }

    FdTable(const FdTable&) = delete;
    FdTable& operator=(const FdTable&) = delete;

    FdTable(FdTable&& other) noexcept : m_entries(other.m_entries), m_capacity(other.m_capacity) {
        other.m_entries = nullptr;
        other.m_capacity = 0U;
    }

    FdTable& operator=(FdTable&& other) noexcept {
        if (this != &other) {
            destroy();
            m_entries = other.m_entries;
            m_capacity = other.m_capacity;
            other.m_entries = nullptr;
            other.m_capacity = 0U;
        }
        return *this;
    }

    [[nodiscard]] bool init(size_t initial_capacity = INITIAL_FD_CAPACITY) noexcept;
    void destroy() noexcept;

    [[nodiscard]] bool ensure_capacity(size_t required_slots, uint64_t max_fds) noexcept;
    [[nodiscard]] int allocate(uint64_t max_fds, size_t min_fd = 0U) noexcept;
    bool install(size_t fd_idx, Kernel::FS::File* file, uint32_t fd_flags = 0U) noexcept;

    [[nodiscard]] FileDescriptor* at(size_t fd_idx) noexcept;
    [[nodiscard]] const FileDescriptor* at(size_t fd_idx) const noexcept;
    [[nodiscard]] Kernel::FS::File* get_file(uint64_t raw_fd) const noexcept;

    bool dup_slot(size_t dst_idx, size_t src_idx, bool clear_cloexec) noexcept;
    void close(size_t fd_idx) noexcept;
    void close_cloexec() noexcept;
    [[nodiscard]] bool clone_from(const FdTable& src) noexcept;

    [[nodiscard]] size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool is_initialized() const noexcept { return m_entries != nullptr; }

  private:
    FileDescriptor* m_entries{nullptr};
    size_t m_capacity{0U};
};

} // namespace Kernel::Process
