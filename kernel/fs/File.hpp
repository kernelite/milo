#pragma once

#include "fs/VfsTypes.hpp"

namespace Kernel::FS {

struct FileOperations {
    int64_t (*read)(File* file, void* buf, size_t count, uint64_t* offset){nullptr};
    int64_t (*write)(File* file, const void* buf, size_t count, uint64_t* offset){nullptr};
    int32_t (*open)(Inode* inode, File* file){nullptr};
    int32_t (*release)(Inode* inode, File* file){nullptr};
    int64_t (*lseek)(File* file, int64_t offset, SeekWhence whence){nullptr};
    int32_t (*ioctl)(File* file, uint64_t request, uint64_t arg){nullptr};
    int32_t (*fsync)(File* file, int32_t datasync){nullptr};
    int32_t (*readdir)(File* file, DirEntryInfo* out_entry){nullptr};
    int16_t (*poll)(File* file, int16_t events){nullptr};
};

class File final {
  public:
    constexpr File() noexcept = default;
    ~File() noexcept = default;

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&&) = delete;
    File& operator=(File&&) = delete;

    void reset() noexcept {
        m_inode = nullptr;
        m_dentry = nullptr;
        m_offset = 0U;
        m_flags = 0U;
        m_refcount = 0U;
        m_f_ops = nullptr;
        m_private_data = nullptr;
        m_active = false;
    }

    void activate() noexcept {
        reset();
        m_refcount = 1U;
        m_active = true;
    }

    void retain() noexcept;
    void release() noexcept;

    [[nodiscard]] constexpr Inode* get_inode() const noexcept { return m_inode; }
    constexpr void set_inode(Inode* new_inode) noexcept { m_inode = new_inode; }

    [[nodiscard]] constexpr Dentry* get_dentry() const noexcept { return m_dentry; }
    constexpr void set_dentry(Dentry* new_dentry) noexcept { m_dentry = new_dentry; }

    [[nodiscard]] constexpr uint64_t get_offset() const noexcept { return m_offset; }
    constexpr void set_offset(uint64_t new_offset) noexcept { m_offset = new_offset; }

    [[nodiscard]] constexpr uint32_t get_flags() const noexcept { return m_flags; }
    constexpr void set_flags(uint32_t new_flags) noexcept { m_flags = new_flags; }
    constexpr void set_status_flags(uint32_t status_flags) noexcept {
        constexpr uint32_t kMutableStatusMask = kOpenNonBlock | kOpenAppend;
        m_flags = (m_flags & ~kMutableStatusMask) | (status_flags & kMutableStatusMask);
    }

    [[nodiscard]] constexpr const FileOperations* get_f_ops() const noexcept { return m_f_ops; }
    constexpr void set_f_ops(const FileOperations* new_ops) noexcept { m_f_ops = new_ops; }

    [[nodiscard]] constexpr void* get_private_data() const noexcept { return m_private_data; }
    constexpr void set_private_data(void* new_data) noexcept { m_private_data = new_data; }

    [[nodiscard]] constexpr uint32_t get_refcount() const noexcept { return m_refcount; }
    constexpr void set_refcount(uint32_t new_refcount) noexcept { m_refcount = new_refcount; }

    [[nodiscard]] constexpr bool is_active() const noexcept { return m_active; }
    constexpr void set_active(bool active) noexcept { m_active = active; }

    [[nodiscard]] int64_t read(void* buf, size_t count) noexcept;
    [[nodiscard]] int64_t write(const void* buf, size_t count) noexcept;
    [[nodiscard]] int64_t seek(int64_t delta, SeekWhence whence) noexcept;
    [[nodiscard]] int32_t ioctl(uint64_t request, uint64_t arg) noexcept;
    [[nodiscard]] int16_t poll(int16_t events) noexcept;
    [[nodiscard]] int32_t readdir(DirEntryInfo* out_entry) noexcept;
    [[nodiscard]] int32_t fsync(int32_t datasync = 0) noexcept;

  private:
    Inode* m_inode{nullptr};
    Dentry* m_dentry{nullptr};
    uint64_t m_offset{0U};
    uint32_t m_flags{0U};
    uint32_t m_refcount{0U};
    const FileOperations* m_f_ops{nullptr};
    void* m_private_data{nullptr};
    bool m_active{false};
};

} // namespace Kernel::FS
