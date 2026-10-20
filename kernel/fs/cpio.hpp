#pragma once

#include <cstddef>
#include <cstdint>

// Packed 110-byte ASCII header conforming to POSIX / SVR4 portable CPIO
struct [[gnu::packed]] CpioNewcHeader {
    char c_magic[6];    // "070701" (no CRC) or "070702" (CRC)
    char c_ino[8];      // File inode number
    char c_mode[8];     // Permissions & file type
    char c_uid[8];      // User ID
    char c_gid[8];      // Group ID
    char c_nlink[8];    // Number of hard links
    char c_mtime[8];    // Modification time
    char c_filesize[8]; // File size in bytes
    char c_maj[8];      // Major device number
    char c_min[8];      // Minor device number
    char c_rmaj[8];     // Remote major device number
    char c_rmin[8];     // Remote minor device number
    char c_namesize[8]; // Pathname length (including null terminator)
    char c_check[8];    // Checksum (for 070702)
};
static_assert(sizeof(CpioNewcHeader) == 110, "CpioNewcHeader must be exactly 110 bytes");

// Constant magic strings
inline constexpr const char CPIO_NEWC_MAGIC_NOCRC[] = "070701";
inline constexpr const char CPIO_NEWC_MAGIC_CRC[] = "070702";
inline constexpr const char CPIO_TRAILER_NAME[] = "TRAILER!!!";

// Standard file type masks
inline constexpr uint32_t CPIO_MODE_FILE_TYPE_MASK = 0170000;
inline constexpr uint32_t CPIO_MODE_DIR = 0040000;
inline constexpr uint32_t CPIO_MODE_REGULAR = 0100000;
inline constexpr uint32_t CPIO_MODE_SYMLINK = 0120000;

// Explicit error results
enum class CpioStatus {
    Success = 0,
    ArchiveTooSmall,
    InvalidMagic,
    MalformedHeader,
    CorruptedPadding,
    PathTooLong,
    FileNotFound,
    UnexpectedEndOfArchive
};

// Non-owning file metadata handle
struct CpioEntry {
    const char* name;     // Null-terminated filename pointer inside archive
    size_t name_len;      // Length of filename (excluding null terminator)
    const uint8_t* data;  // Pointer to raw payload bytes inside archive
    size_t size;          // File size in bytes
    uint32_t mode;        // Decoded file mode (permissions + type)
    bool is_directory;    // True if mode matches CPIO_MODE_DIR
    bool is_regular_file; // True if mode matches CPIO_MODE_REGULAR
};

// Alignment calculation (4-byte alignment enforcement)
constexpr size_t cpio_align_up(size_t val) noexcept {
    return (val + 3U) & ~3U;
}

// Freestanding, branch-light 8-character hex decoder
constexpr bool parse_hex8(const char* src, uint32_t& out_val) noexcept {
    if (src == nullptr) {
        return false;
    }
    uint32_t val = 0;
    for (size_t i = 0; i < 8; ++i) {
        const char chr = src[i];
        uint32_t dgt = 0;
        if (chr >= '0' && chr <= '9') {
            dgt = static_cast<uint32_t>(chr - '0');
        } else if (chr >= 'a' && chr <= 'f') {
            dgt = static_cast<uint32_t>(chr - 'a' + 10);
        } else if (chr >= 'A' && chr <= 'F') {
            dgt = static_cast<uint32_t>(chr - 'A' + 10);
        } else {
            return false;
        }
        val = (val << 4U) | dgt;
    }
    out_val = val;
    return true;
}

// Path normalization logic: strips leading '/' and './'
constexpr const char* cpio_normalize_path(const char* path) noexcept {
    if (path == nullptr) {
        return "";
    }
    while (true) {
        if (path[0] == '/') {
            path++;
        } else if (path[0] == '.' && path[1] == '/') {
            path += 2;
        } else {
            break;
        }
    }
    return path;
}

// Matches normalized entry name against normalized search path
inline bool cpio_path_matches(const char* entry_name, const char* search_path) noexcept {
    const char* path1 = cpio_normalize_path(entry_name);
    const char* path2 = cpio_normalize_path(search_path);
    while (*path1 != '\0' && *path2 != '\0' && *path1 == *path2) {
        path1++;
        path2++;
    }
    return *path1 == *path2;
}

const char* cpio_status_str(CpioStatus status) noexcept;

namespace detail {
template <typename T> struct is_bool { static constexpr bool value = false; };
template <> struct is_bool<bool> { static constexpr bool value = true; };
} // namespace detail

class CpioArchive {
  public:
    constexpr CpioArchive(const void* base, size_t size) noexcept
        : m_base(static_cast<const uint8_t*>(base)), m_size(size) {}

    // Validates header magic and returns false if corrupted
    bool is_valid() const noexcept;

    // Iterates sequentially over all valid entries until TRAILER!!! or end
    template <typename Func> CpioStatus for_each(Func&& callback) const noexcept {
        if (m_base == nullptr || m_size < sizeof(CpioNewcHeader)) {
            return CpioStatus::ArchiveTooSmall;
        }

        size_t current_offset = 0;

        while (current_offset < m_size) {
            // Verify entry starts on a 4-byte boundary
            if ((current_offset & 3U) != 0U) {
                return CpioStatus::CorruptedPadding;
            }

            // Step 1: Verify at least 110 bytes remain
            if (m_size - current_offset < sizeof(CpioNewcHeader)) {
                return CpioStatus::UnexpectedEndOfArchive;
            }

            const auto* header = reinterpret_cast<const CpioNewcHeader*>(m_base + current_offset);

            // Step 2: Verify magic is "070701" or "070702"
            if (header->c_magic[0] != '0' || header->c_magic[1] != '7' ||
                header->c_magic[2] != '0' || header->c_magic[3] != '7' ||
                header->c_magic[4] != '0' ||
                (header->c_magic[5] != '1' && header->c_magic[5] != '2')) {
                return CpioStatus::InvalidMagic;
            }

            // Step 3: Parse c_namesize, c_filesize, and c_mode from hex
            uint32_t namesize = 0;
            uint32_t filesize = 0;
            uint32_t mode = 0;
            if (!parse_hex8(header->c_namesize, namesize) ||
                !parse_hex8(header->c_filesize, filesize) || !parse_hex8(header->c_mode, mode)) {
                return CpioStatus::MalformedHeader;
            }

            // Step 4: Ensure namesize > 0 and namesize < 4096
            if (namesize == 0) {
                return CpioStatus::MalformedHeader;
            }
            if (namesize >= 4096) {
                return CpioStatus::PathTooLong;
            }

            // Step 5: Check name buffer containment
            if (m_size - (current_offset + sizeof(CpioNewcHeader)) < namesize) {
                return CpioStatus::UnexpectedEndOfArchive;
            }

            const char* name_ptr =
                reinterpret_cast<const char*>(m_base + current_offset + sizeof(CpioNewcHeader));

            // Must be null-terminated
            if (name_ptr[namesize - 1] != '\0') {
                return CpioStatus::MalformedHeader;
            }

            // Step 6: Calculate data offset with 4-byte alignment
            const size_t name_end_offset = current_offset + sizeof(CpioNewcHeader) + namesize;
            const size_t payload_offset = cpio_align_up(name_end_offset);

            if (payload_offset > m_size) {
                return CpioStatus::UnexpectedEndOfArchive;
            }

            // Bounds-checked zero padding check between name and payload
            for (size_t ptr = name_end_offset; ptr < payload_offset; ++ptr) {
                if (m_base[ptr] != 0U) {
                    return CpioStatus::CorruptedPadding;
                }
            }

            // Step 7: Check payload containment
            if (m_size - payload_offset < filesize) {
                return CpioStatus::UnexpectedEndOfArchive;
            }

            // Step 8: Calculate next entry offset with 4-byte alignment
            const size_t payload_end_offset = payload_offset + filesize;
            const size_t next_offset = cpio_align_up(payload_end_offset);

            // Step 9: If entry name equals "TRAILER!!!", terminate iteration cleanly
            if (namesize == 11U) {
                bool is_trailer = true;
                for (size_t i = 0; i < 11U; ++i) {
                    if (name_ptr[i] != CPIO_TRAILER_NAME[i]) {
                        is_trailer = false;
                        break;
                    }
                }
                if (is_trailer) {
                    return CpioStatus::Success;
                }
            }

            if (next_offset > m_size) {
                return CpioStatus::UnexpectedEndOfArchive;
            }

            // Bounds-checked zero padding check between payload and next record
            for (size_t ptr = payload_end_offset; ptr < next_offset; ++ptr) {
                if (m_base[ptr] != 0U) {
                    return CpioStatus::CorruptedPadding;
                }
            }

            // Construct non-owning file view
            const CpioEntry entry{
                .name = name_ptr,
                .name_len = static_cast<size_t>(namesize - 1),
                .data = m_base + payload_offset,
                .size = static_cast<size_t>(filesize),
                .mode = mode,
                .is_directory = (mode & CPIO_MODE_FILE_TYPE_MASK) == CPIO_MODE_DIR,
                .is_regular_file = (mode & CPIO_MODE_FILE_TYPE_MASK) == CPIO_MODE_REGULAR,
            };

            if constexpr (detail::is_bool<decltype(callback(entry))>::value) {
                if (!callback(entry)) {
                    return CpioStatus::Success; // Early-exit requested
                }
            } else {
                callback(entry);
            }

            current_offset = next_offset;
        }

        return CpioStatus::UnexpectedEndOfArchive;
    }

    // Finds an entry by relative/absolute path
    CpioStatus lookup(const char* path, CpioEntry& out_entry) const noexcept;

    constexpr const uint8_t* base() const noexcept { return m_base; }
    constexpr size_t size() const noexcept { return m_size; }

  private:
    const uint8_t* m_base{nullptr};
    size_t m_size{0};
};

inline CpioStatus cpio_lookup(const void* base, size_t size, const char* path,
                              CpioEntry& out_entry) noexcept {
    return CpioArchive(base, size).lookup(path, out_entry);
}