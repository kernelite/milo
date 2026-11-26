#include "process/FdTable.hpp"

#include "fs/Vfs.hpp"
#include "mm/heap.hpp"

#include <new>

namespace Kernel::Process {

namespace {

constexpr int ERR_EMFILE = -24;

} // namespace

bool FdTable::init(size_t initial_capacity) noexcept {
    if (m_entries != nullptr) {
        destroy();
    }
    if (initial_capacity == 0U) {
        initial_capacity = INITIAL_FD_CAPACITY;
    }

    auto* const table = static_cast<FileDescriptor*>(
        MM::kzalloc(initial_capacity * sizeof(FileDescriptor), alignof(FileDescriptor)));
    if (table == nullptr) {
        return false;
    }

    for (size_t idx = 0U; idx < initial_capacity; ++idx) {
        new (&table[idx]) FileDescriptor{};
    }

    m_entries = table;
    m_capacity = initial_capacity;
    return true;
}

void FdTable::destroy() noexcept {
    if (m_entries == nullptr) {
        m_capacity = 0U;
        return;
    }

    for (size_t idx = 0U; idx < m_capacity; ++idx) {
        if (m_entries[idx].is_open()) {
            close(idx);
        }
    }

    MM::kfree(m_entries);
    m_entries = nullptr;
    m_capacity = 0U;
}

bool FdTable::ensure_capacity(size_t required_slots, uint64_t max_fds) noexcept {
    if (required_slots > max_fds) {
        return false;
    }
    if (m_entries != nullptr && required_slots <= m_capacity) {
        return true;
    }

    size_t new_capacity = (m_capacity > 0U) ? m_capacity : INITIAL_FD_CAPACITY;
    while (new_capacity < required_slots) {
        if (new_capacity >= max_fds) {
            new_capacity = static_cast<size_t>(max_fds);
            break;
        }
        if (new_capacity > (SIZE_MAX / 2U)) {
            new_capacity = static_cast<size_t>(max_fds);
            break;
        }
        const size_t doubled = new_capacity * 2U;
        new_capacity = (doubled > max_fds) ? static_cast<size_t>(max_fds) : doubled;
    }

    auto* const grown = static_cast<FileDescriptor*>(
        MM::krealloc(m_entries, new_capacity * sizeof(FileDescriptor), alignof(FileDescriptor)));
    if (grown == nullptr) {
        return false;
    }

    for (size_t idx = m_capacity; idx < new_capacity; ++idx) {
        new (&grown[idx]) FileDescriptor{};
    }

    m_entries = grown;
    m_capacity = new_capacity;
    return true;
}

int FdTable::allocate(uint64_t max_fds, size_t min_fd) noexcept {
    const auto limit = static_cast<size_t>(max_fds);
    if (min_fd >= limit) {
        return ERR_EMFILE;
    }

    if (m_entries == nullptr || min_fd >= m_capacity) {
        if (!ensure_capacity(min_fd + 1U, max_fds)) {
            return ERR_EMFILE;
        }
    }

    const size_t search_limit = (m_capacity < limit) ? m_capacity : limit;
    for (size_t idx = min_fd; idx < search_limit; ++idx) {
        if (!m_entries[idx].is_open()) {
            return static_cast<int>(idx);
        }
    }

    if (m_capacity >= limit) {
        return ERR_EMFILE;
    }

    const size_t next_slot = m_capacity;
    if (!ensure_capacity(next_slot + 1U, max_fds)) {
        return ERR_EMFILE;
    }

    return static_cast<int>(next_slot);
}

bool FdTable::install(size_t fd_idx, Kernel::FS::File* file, uint32_t fd_flags) noexcept {
    if (m_entries == nullptr || fd_idx >= m_capacity) {
        return false;
    }
    if (m_entries[fd_idx].is_open()) {
        close(fd_idx);
    }
    m_entries[fd_idx] = FileDescriptor(file, fd_flags);
    return true;
}

FileDescriptor* FdTable::at(size_t fd_idx) noexcept {
    if (m_entries == nullptr || fd_idx >= m_capacity) {
        return nullptr;
    }
    return &m_entries[fd_idx];
}

const FileDescriptor* FdTable::at(size_t fd_idx) const noexcept {
    if (m_entries == nullptr || fd_idx >= m_capacity) {
        return nullptr;
    }
    return &m_entries[fd_idx];
}

Kernel::FS::File* FdTable::get_file(uint64_t raw_fd) const noexcept {
    if (m_entries == nullptr || raw_fd >= m_capacity) {
        return nullptr;
    }
    const auto& desc = m_entries[static_cast<size_t>(raw_fd)];
    return desc.is_open() ? desc.file() : nullptr;
}

bool FdTable::dup_slot(size_t dst_idx, size_t src_idx, bool clear_cloexec) noexcept {
    if (m_entries == nullptr || dst_idx >= m_capacity || src_idx >= m_capacity) {
        return false;
    }
    if (dst_idx == src_idx) {
        return true;
    }
    if (m_entries[dst_idx].is_open()) {
        close(dst_idx);
    }

    FileDescriptor copied = m_entries[src_idx];
    if (clear_cloexec) {
        copied.set_cloexec(false);
    }
    if (copied.is_open()) {
        Kernel::FS::file_get(copied.file());
    }
    m_entries[dst_idx] = copied;
    return true;
}

void FdTable::close(size_t fd_idx) noexcept {
    if (m_entries == nullptr || fd_idx >= m_capacity) {
        return;
    }

    auto& desc = m_entries[fd_idx];
    if (!desc.is_open()) {
        desc.reset();
        return;
    }

    (void)Kernel::FS::vfs_close(desc.file());
    desc.reset();
}

void FdTable::close_cloexec() noexcept {
    if (m_entries == nullptr) {
        return;
    }
    for (size_t idx = 0U; idx < m_capacity; ++idx) {
        if (m_entries[idx].is_open() && m_entries[idx].has_cloexec()) {
            close(idx);
        }
    }
}

bool FdTable::clone_from(const FdTable& src) noexcept {
    if (src.m_entries == nullptr || src.m_capacity == 0U) {
        destroy();
        return true;
    }

    for (size_t idx = 0U; idx < m_capacity; ++idx) {
        if (m_entries[idx].is_open()) {
            close(idx);
        }
    }

    if (m_capacity < src.m_capacity) {
        if (!ensure_capacity(src.m_capacity, src.m_capacity)) {
            return false;
        }
    }

    for (size_t idx = 0U; idx < src.m_capacity; ++idx) {
        const FileDescriptor& src_desc = src.m_entries[idx];
        m_entries[idx] = src_desc;
        if (src_desc.is_open()) {
            Kernel::FS::file_get(src_desc.file());
        }
    }
    return true;
}

} // namespace Kernel::Process
