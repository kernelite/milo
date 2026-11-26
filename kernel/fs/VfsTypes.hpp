#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::FS {

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

// Zero-overhead intrusive RAII smart pointer for VFS types providing retain()/release().
template <typename T> class RefPtr final {
  public:
    constexpr RefPtr() noexcept = default;
    // NOLINTNEXTLINE(hicpp-explicit-conversions)
    constexpr RefPtr([[maybe_unused]] decltype(nullptr) null_ptr) noexcept {}

    [[nodiscard]] static RefPtr adopt(T* raw) noexcept { return RefPtr(raw, AdoptTag{}); }

    [[nodiscard]] static RefPtr retain_from(T* raw) noexcept {
        if (raw != nullptr) {
            raw->retain();
        }
        return RefPtr(raw, AdoptTag{});
    }

    ~RefPtr() noexcept { reset(); }

    RefPtr(const RefPtr& other) noexcept : m_ptr(other.m_ptr) {
        if (m_ptr != nullptr) {
            m_ptr->retain();
        }
    }

    RefPtr& operator=(const RefPtr& other) noexcept {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            if (m_ptr != nullptr) {
                m_ptr->retain();
            }
        }
        return *this;
    }

    RefPtr(RefPtr&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }

    RefPtr& operator=(RefPtr&& other) noexcept {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    RefPtr& operator=([[maybe_unused]] decltype(nullptr) null_ptr) noexcept {
        reset();
        return *this;
    }

    void reset() noexcept {
        if (m_ptr != nullptr) {
            m_ptr->release();
            m_ptr = nullptr;
        }
    }

    [[nodiscard]] T* detach() noexcept {
        T* const raw = m_ptr;
        m_ptr = nullptr;
        return raw;
    }

    [[nodiscard]] constexpr T* get() const noexcept { return m_ptr; }
    [[nodiscard]] constexpr T* operator->() const noexcept { return m_ptr; }
    [[nodiscard]] constexpr T& operator*() const noexcept { return *m_ptr; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return m_ptr != nullptr; }

    [[nodiscard]] constexpr bool operator==(const RefPtr& other) const noexcept {
        return m_ptr == other.m_ptr;
    }
    [[nodiscard]] constexpr bool operator!=(const RefPtr& other) const noexcept {
        return m_ptr != other.m_ptr;
    }
    [[nodiscard]] constexpr bool operator==(const T* raw) const noexcept { return m_ptr == raw; }
    [[nodiscard]] constexpr bool operator!=(const T* raw) const noexcept { return m_ptr != raw; }
    [[nodiscard]] constexpr bool
    operator==([[maybe_unused]] decltype(nullptr) null_ptr) const noexcept {
        return m_ptr == nullptr;
    }
    [[nodiscard]] constexpr bool
    operator!=([[maybe_unused]] decltype(nullptr) null_ptr) const noexcept {
        return m_ptr != nullptr;
    }

  private:
    struct AdoptTag {};
    constexpr RefPtr(T* raw, [[maybe_unused]] AdoptTag tag) noexcept : m_ptr(raw) {}

    T* m_ptr{nullptr};
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

class Superblock;
class Inode;
class Dentry;
class File;
struct FileOperations;

struct DirEntryInfo {
    uint64_t ino{0U};
    uint32_t type{0U};
    char name[kMaxNameLen + 1U]{};
};

} // namespace Kernel::FS