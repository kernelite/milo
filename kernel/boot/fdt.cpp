#include "boot/fdt.hpp"

namespace {

// Token definitions from devicetree specification
constexpr uint32_t FDT_BEGIN_NODE = 0x00000001;
constexpr uint32_t FDT_END_NODE = 0x00000002;
constexpr uint32_t FDT_PROP = 0x00000003;
constexpr uint32_t FDT_NOP = 0x00000004;
constexpr uint32_t FDT_END = 0x00000009;

// Safe Big-Endian readers that prevent alignment faults
inline uint32_t read_be32(const void* ptr) noexcept {
    const auto* p = static_cast<const uint8_t*>(ptr);
    return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) |
           (static_cast<uint32_t>(p[2]) << 8U) | (static_cast<uint32_t>(p[3]) << 0U);
}

inline uint64_t read_be64(const void* ptr) noexcept {
    const auto* p = static_cast<const uint8_t*>(ptr);
    return (static_cast<uint64_t>(read_be32(p)) << 32U) | static_cast<uint64_t>(read_be32(p + 4));
}

inline size_t str_len(const char* s) noexcept {
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

inline bool str_eq(const char* s1, const char* s2) noexcept {
    if (s1 == nullptr || s2 == nullptr) {
        return s1 == s2;
    }
    while (*s1 != '\0' && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *s1 == *s2;
}

inline uintptr_t align_up(uintptr_t val, uintptr_t alignment) noexcept {
    return (val + alignment - 1U) & ~(alignment - 1U);
}

} // namespace

namespace FDT {

bool is_valid(uintptr_t fdt_paddr) noexcept {
    if (fdt_paddr == 0 || (fdt_paddr & 0x3U) != 0) {
        return false;
    }
    const auto* raw_ptr = reinterpret_cast<const void*>(fdt_paddr);
    return read_be32(raw_ptr) == FDT_MAGIC;
}

uint32_t get_total_size(uintptr_t fdt_paddr) noexcept {
    if (!is_valid(fdt_paddr)) {
        return 0;
    }
    const auto* hdr = reinterpret_cast<const Header*>(fdt_paddr);
    return read_be32(&hdr->totalsize);
}

InitrdLocation find_initrd(uintptr_t fdt_paddr) noexcept {
    InitrdLocation loc{};
    if (!is_valid(fdt_paddr)) {
        return loc;
    }

    const auto* hdr = reinterpret_cast<const Header*>(fdt_paddr);
    const uint32_t totalsize = read_be32(&hdr->totalsize);
    const uint32_t off_struct = read_be32(&hdr->off_dt_struct);
    const uint32_t off_strings = read_be32(&hdr->off_dt_strings);
    const uint32_t size_struct = read_be32(&hdr->size_dt_struct);

    if (off_struct >= totalsize || off_strings >= totalsize) {
        return loc;
    }

    const auto* base = reinterpret_cast<const uint8_t*>(fdt_paddr);
    const char* strings_block = reinterpret_cast<const char*>(base + off_strings);

    const uint8_t* p = base + off_struct;
    const uint8_t* struct_end = p + size_struct;

    int depth = 0;
    bool in_chosen = false;
    int chosen_depth = -1;

    uintptr_t initrd_start = 0;
    uintptr_t initrd_end = 0;
    bool has_start = false;
    bool has_end = false;

    while (p + sizeof(uint32_t) <= struct_end) {
        const uint32_t token = read_be32(p);
        p += sizeof(uint32_t);

        if (token == FDT_BEGIN_NODE) {
            depth++;
            const char* node_name = reinterpret_cast<const char*>(p);
            const size_t name_len = str_len(node_name);

            // Node name is null-terminated and padded to 4 bytes
            p = reinterpret_cast<const uint8_t*>(
                align_up(reinterpret_cast<uintptr_t>(p + name_len + 1), 4));

            // In FDT, root node is at depth 1 with name ""
            // /chosen is a direct child of root (depth 2)
            if (depth == 2 && str_eq(node_name, "chosen")) {
                in_chosen = true;
                chosen_depth = depth;
            }
        } else if (token == FDT_END_NODE) {
            if (in_chosen && depth == chosen_depth) {
                in_chosen = false;
                chosen_depth = -1;
            }
            depth--;
        } else if (token == FDT_PROP) {
            if (p + 2 * sizeof(uint32_t) > struct_end) {
                break;
            }

            const uint32_t prop_len = read_be32(p);
            const uint32_t nameoff = read_be32(p + 4);
            p += 8;

            const char* prop_name = strings_block + nameoff;
            const uint8_t* prop_val = p;

            // Property value payload is padded to 4 bytes
            p = reinterpret_cast<const uint8_t*>(
                align_up(reinterpret_cast<uintptr_t>(p + prop_len), 4));

            if (in_chosen) {
                if (str_eq(prop_name, "linux,initrd-start")) {
                    if (prop_len == 4) {
                        initrd_start = static_cast<uintptr_t>(read_be32(prop_val));
                    } else if (prop_len == 8) {
                        initrd_start = static_cast<uintptr_t>(read_be64(prop_val));
                    }
                    has_start = true;
                } else if (str_eq(prop_name, "linux,initrd-end")) {
                    if (prop_len == 4) {
                        initrd_end = static_cast<uintptr_t>(read_be32(prop_val));
                    } else if (prop_len == 8) {
                        initrd_end = static_cast<uintptr_t>(read_be64(prop_val));
                    }
                    has_end = true;
                }
            }
        } else if (token == FDT_NOP) {
            // No payload
            continue;
        } else if (token == FDT_END) {
            break;
        } else {
            // Malformed token encountered; abort parse
            break;
        }
    }

    if (has_start && has_end && initrd_end >= initrd_start) {
        loc.start_paddr = initrd_start;
        loc.end_paddr = initrd_end;
        loc.size = initrd_end - initrd_start;
        loc.valid = true;
    }

    return loc;
}

} // namespace FDT