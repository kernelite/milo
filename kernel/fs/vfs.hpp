#pragma once

#include <cstddef>
#include <cstdint>

namespace kernel::fs {

// Lightweight freestanding string slice (avoids hosted libstdc++ <string_view> terminate hooks).
class StringView {
  public:
    constexpr StringView() noexcept = default;
    constexpr StringView(const char* str, size_t len) noexcept : m_data(str), m_size(len) {}
    // NOLINTNEXTLINE(hicpp-explicit-conversions)
    constexpr StringView(const char* str) noexcept
        : m_data(str), m_size(str != nullptr ? cstr_len(str) : 0U) {}

    [[nodiscard]] constexpr const char* data() const noexcept { return m_data; }
    [[nodiscard]] constexpr size_t size() const noexcept { return m_size; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_size == 0U; }

    [[nodiscard]] constexpr char operator[](size_t index) const noexcept { return m_data[index]; }
    [[nodiscard]] constexpr char front() const noexcept { return m_data[0]; }
    [[nodiscard]] constexpr char back() const noexcept { return m_data[m_size - 1U]; }

    [[nodiscard]] constexpr StringView substr(size_t pos, size_t count) const noexcept {
        if (pos >= m_size) {
            return {};
        }
        const size_t avail = m_size - pos;
        const size_t len = (count < avail) ? count : avail;
        return {m_data + pos, len};
    }

    [[nodiscard]] constexpr size_t rfind(char target, size_t pos) const noexcept {
        if (m_size == 0U) {
            return kNpos;
        }
        size_t idx = (pos < m_size) ? pos : (m_size - 1U);
        while (true) {
            if (m_data[idx] == target) {
                return idx;
            }
            if (idx == 0U) {
                break;
            }
            --idx;
        }
        return kNpos;
    }

    [[nodiscard]] constexpr bool operator==(StringView other) const noexcept {
        if (m_size != other.m_size) {
            return false;
        }
        for (size_t idx = 0U; idx < m_size; ++idx) {
            if (m_data[idx] != other.m_data[idx]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr bool operator!=(StringView other) const noexcept {
        return !(*this == other);
    }

    static constexpr size_t kNpos = static_cast<size_t>(-1);

  private:
    static constexpr size_t cstr_len(const char* str) noexcept {
        size_t len = 0U;
        while (str[len] != '\0') {
            ++len;
        }
        return len;
    }

    const char* m_data{nullptr};
    size_t m_size{0U};
};

// POSIX-compatible negative errno constants (freestanding-safe).
inline constexpr int32_t kOk = 0;
inline constexpr int32_t kEperm = -1;
inline constexpr int32_t kEnoent = -2;
inline constexpr int32_t kEintr = -4;
inline constexpr int32_t kEio = -5;
inline constexpr int32_t kEbadf = -9;
inline constexpr int32_t kEagain = -11;
inline constexpr int32_t kEnomem = -12;
inline constexpr int32_t kEacces = -13;
inline constexpr int32_t kEfault = -14;
inline constexpr int32_t kEbusy = -16;
inline constexpr int32_t kEexist = -17;
inline constexpr int32_t kExdev = -18;
inline constexpr int32_t kEnotdir = -20;
inline constexpr int32_t kEisdir = -21;
inline constexpr int32_t kEinval = -22;
inline constexpr int32_t kEnfile = -23;
inline constexpr int32_t kEmfile = -24;
inline constexpr int32_t kEnotty = -25;
inline constexpr int32_t kEnospc = -28;
inline constexpr int32_t kEspipe = -29;
inline constexpr int32_t kErofs = -30;
inline constexpr int32_t kEpipe = -32;
inline constexpr int32_t kEnametoolong = -36;
inline constexpr int32_t kEnosys = -38;
inline constexpr int32_t kEnotempty = -39;
inline constexpr int32_t kEloop = -40;
inline constexpr int32_t kEopnotsupp = -95;

// POSIX file mode type masks and constants.
inline constexpr uint32_t kModeTypeMask = 0170000U;
inline constexpr uint32_t kModeFifo = 0010000U;
inline constexpr uint32_t kModeChr = 0020000U;
inline constexpr uint32_t kModeDir = 0040000U;
inline constexpr uint32_t kModeBlk = 0060000U;
inline constexpr uint32_t kModeReg = 0100000U;
inline constexpr uint32_t kModeLnk = 0120000U;

// Open flags.
inline constexpr uint32_t kOpenReadOnly = 0x0000U;
inline constexpr uint32_t kOpenWriteOnly = 0x0001U;
inline constexpr uint32_t kOpenReadWrite = 0x0002U;
inline constexpr uint32_t kOpenAccessMask = 0x0003U;
inline constexpr uint32_t kOpenCreate = 0x0040U;
inline constexpr uint32_t kOpenExcl = 0x0080U;
inline constexpr uint32_t kOpenTrunc = 0x0200U;
inline constexpr uint32_t kOpenAppend = 0x0400U;
inline constexpr uint32_t kOpenNonBlock = 0x0800U;
inline constexpr uint32_t kOpenDirectory = 0x10000U;
inline constexpr uint32_t kOpenCloexec = 0x80000U;

// Superblock flags.
inline constexpr uint64_t kSbReadOnly = (1ULL << 0U);

// Dentry flags.
inline constexpr uint32_t kDentryRoot = (1U << 0U);
inline constexpr uint32_t kDentryMounted = (1U << 1U);

// Static pool limits (zero dynamic heap allocations).
inline constexpr size_t kMaxNameLen = 63U;
inline constexpr size_t kMaxSuperblocks = 16U;
inline constexpr size_t kMaxMounts = 16U;
inline constexpr size_t kMaxInodes = 1024U;
inline constexpr size_t kMaxDentries = 1024U;
inline constexpr size_t kMaxFiles = 512U;
inline constexpr size_t kMaxSymlinkDepth = 8U;

enum class SeekWhence : uint8_t {
    kSet = 0U,
    kCur = 1U,
    kEnd = 2U,
};

struct Superblock;
struct Inode;
struct Dentry;
struct File;

struct DirEntryInfo {
    uint64_t ino{0U};
    uint32_t type{0U};
    char name[kMaxNameLen + 1U]{};
};

struct SuperblockOperations {
    int32_t (*sync_fs)(Superblock* sb){nullptr};
    Inode* (*alloc_inode)(Superblock* sb){nullptr};
    void (*destroy_inode)(Inode* inode){nullptr};
};

struct InodeOperations {
    int32_t (*lookup)(Inode* dir, Dentry* target){nullptr};
    int32_t (*create)(Inode* dir, Dentry* target, uint32_t mode){nullptr};
    int32_t (*mkdir)(Inode* dir, Dentry* target, uint32_t mode){nullptr};
    int32_t (*unlink)(Inode* dir, Dentry* target){nullptr};
    int32_t (*rmdir)(Inode* dir, Dentry* target){nullptr};
    int32_t (*rename)(Inode* old_dir, Dentry* old_dentry, Inode* new_dir,
                      Dentry* new_dentry){nullptr};
    int32_t (*truncate)(Inode* inode, uint64_t new_size){nullptr};
    int32_t (*readlink)(Inode* inode, char* buf, size_t buf_size, size_t* out_len){nullptr};
};

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

struct Superblock {
    const char* fs_type{nullptr};
    uint32_t block_size{4096U};
    uint64_t flags{0U};
    Dentry* root{nullptr};
    const SuperblockOperations* ops{nullptr};
    void* fs_private{nullptr};
    bool active{false};

    [[nodiscard]] bool is_read_only() const noexcept { return (flags & kSbReadOnly) != 0U; }
};

struct Inode {
    uint64_t ino{0U};
    uint32_t mode{0U};
    uint32_t uid{0U};
    uint32_t gid{0U};
    uint64_t size{0U};
    uint64_t rdev{0U};
    uint32_t nlink{1U};
    uint64_t atime_ns{0U};
    uint64_t mtime_ns{0U};
    uint64_t ctime_ns{0U};
    uint32_t refcount{0U};
    Superblock* sb{nullptr};
    const InodeOperations* i_ops{nullptr};
    const FileOperations* f_ops{nullptr};
    void* fs_private{nullptr};
    bool active{false};

    [[nodiscard]] bool is_dir() const noexcept { return (mode & kModeTypeMask) == kModeDir; }
    [[nodiscard]] bool is_reg() const noexcept { return (mode & kModeTypeMask) == kModeReg; }
    [[nodiscard]] bool is_chr() const noexcept { return (mode & kModeTypeMask) == kModeChr; }
    [[nodiscard]] bool is_fifo() const noexcept { return (mode & kModeTypeMask) == kModeFifo; }
    [[nodiscard]] bool is_symlink() const noexcept { return (mode & kModeTypeMask) == kModeLnk; }
};

struct Dentry {
    char name[kMaxNameLen + 1U]{};
    uint8_t name_len{0U};
    Inode* inode{nullptr};
    Superblock* sb{nullptr};
    Dentry* parent{nullptr};
    Dentry* first_child{nullptr};
    Dentry* next_sibling{nullptr};
    uint32_t refcount{0U};
    uint32_t flags{0U};
    bool active{false};

    [[nodiscard]] StringView name_view() const noexcept { return {name, name_len}; }
    [[nodiscard]] bool is_mounted() const noexcept { return (flags & kDentryMounted) != 0U; }
    [[nodiscard]] bool is_root() const noexcept { return (flags & kDentryRoot) != 0U; }
};

struct File {
    Inode* inode{nullptr};
    Dentry* dentry{nullptr};
    uint64_t offset{0U};
    uint32_t flags{0U};
    uint32_t refcount{0U};
    const FileOperations* f_ops{nullptr};
    void* private_data{nullptr};
    bool active{false};
};

struct Mount {
    Dentry* mountpoint{nullptr};
    Superblock* sb{nullptr};
    Mount* parent_mount{nullptr};
    bool active{false};
};

struct VfsStats {
    size_t active_superblocks{0U};
    size_t active_mounts{0U};
    size_t active_inodes{0U};
    size_t active_dentries{0U};
    size_t active_files{0U};
};

// Lifecycle & pool management.
void vfs_reset() noexcept;
[[nodiscard]] VfsStats vfs_stats() noexcept;

[[nodiscard]] Superblock* vfs_alloc_superblock() noexcept;
void vfs_free_superblock(Superblock* sb) noexcept;

[[nodiscard]] Inode* vfs_alloc_inode(Superblock* sb) noexcept;
void inode_get(Inode* inode) noexcept;
void inode_put(Inode* inode) noexcept;

[[nodiscard]] Dentry* vfs_alloc_dentry(Superblock* sb, Dentry* parent, StringView name) noexcept;
void dentry_get(Dentry* dentry) noexcept;
void dentry_put(Dentry* dentry) noexcept;
void dentry_attach(Dentry* parent, Dentry* child) noexcept;
void dentry_detach(Dentry* child) noexcept;
[[nodiscard]] Dentry* dentry_find_child(Dentry* parent, StringView name) noexcept;

[[nodiscard]] File* vfs_alloc_file() noexcept;
void file_get(File* file) noexcept;
void file_put(File* file) noexcept;

// Mount table management.
[[nodiscard]] int32_t vfs_mount_root(Superblock* root_sb) noexcept;
[[nodiscard]] int32_t vfs_mount(StringView target_path, Superblock* sb) noexcept;
[[nodiscard]] int32_t vfs_umount(StringView target_path) noexcept;
[[nodiscard]] Dentry* vfs_root_dentry() noexcept;

// Path resolution (namei).
[[nodiscard]] int32_t vfs_namei(StringView path, Dentry* cwd, Dentry** out_dentry,
                                bool follow_symlinks = true) noexcept;
[[nodiscard]] int32_t vfs_namei_parent(StringView path, Dentry* cwd, Dentry** out_parent,
                                       StringView* out_leaf,
                                       bool* out_trailing_slash = nullptr) noexcept;
[[nodiscard]] int32_t vfs_readlink(StringView path, Dentry* cwd, char* buf, size_t buf_size,
                                   size_t* out_len) noexcept;

// High-level VFS file & namespace operations.
[[nodiscard]] int32_t vfs_open_at(Dentry* cwd, StringView path, uint32_t flags, uint32_t mode,
                                  File** out_file) noexcept;
[[nodiscard]] int32_t vfs_open(StringView path, uint32_t flags, uint32_t mode,
                               File** out_file) noexcept;
[[nodiscard]] int32_t vfs_close(File* file) noexcept;
[[nodiscard]] int64_t vfs_read(File* file, void* buf, size_t count) noexcept;
[[nodiscard]] int64_t vfs_write(File* file, const void* buf, size_t count) noexcept;
[[nodiscard]] int64_t vfs_lseek(File* file, int64_t offset, SeekWhence whence) noexcept;
[[nodiscard]] int32_t vfs_ioctl(File* file, uint64_t request, uint64_t arg) noexcept;
[[nodiscard]] int16_t vfs_poll(File* file, int16_t events) noexcept;
[[nodiscard]] int32_t vfs_readdir(File* file, DirEntryInfo* out_entry) noexcept;
[[nodiscard]] int32_t vfs_mkdir_at(Dentry* cwd, StringView path, uint32_t mode) noexcept;
[[nodiscard]] int32_t vfs_mkdir(StringView path, uint32_t mode) noexcept;
[[nodiscard]] int32_t vfs_unlink_at(Dentry* cwd, StringView path) noexcept;
[[nodiscard]] int32_t vfs_unlink(StringView path) noexcept;
[[nodiscard]] int32_t vfs_rmdir_at(Dentry* cwd, StringView path) noexcept;
[[nodiscard]] int32_t vfs_rmdir(StringView path) noexcept;
[[nodiscard]] int32_t vfs_rename_at(Dentry* old_cwd, StringView old_path, Dentry* new_cwd,
                                    StringView new_path) noexcept;
[[nodiscard]] int32_t vfs_rename(StringView old_path, StringView new_path) noexcept;

} // namespace kernel::fs