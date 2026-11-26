#include "boot/fdt.hpp"

#include "hal/console.hpp"
#include "memory_config.hpp"

#include <cstddef>
#include <cstdint>

namespace {

constexpr uint32_t FDT_MAGIC = 0xD00DFEEDU;
constexpr size_t FDT_HEADER_SIZE = 40U;
constexpr size_t FDT_MAX_SANE_SIZE = 256UL * 1024UL * 1024UL;

constexpr uint32_t FDT_BEGIN_NODE = 0x00000001U;
constexpr uint32_t FDT_END_NODE = 0x00000002U;
constexpr uint32_t FDT_PROP = 0x00000003U;
constexpr uint32_t FDT_NOP = 0x00000004U;
constexpr uint32_t FDT_END = 0x00000009U;

constexpr size_t FALLBACK_RAM_SIZE = 256UL * 1024UL * 1024UL;

void fdt_print(const char* str) noexcept {
    size_t len = 0U;
    while (str[len] != '\0') {
        ++len;
    }
    HAL::get_console().write(str, len);
}

inline uint32_t read_be32(const uint8_t* ptr) noexcept {
    return (static_cast<uint32_t>(ptr[0]) << 24U) | (static_cast<uint32_t>(ptr[1]) << 16U) |
           (static_cast<uint32_t>(ptr[2]) << 8U) | static_cast<uint32_t>(ptr[3]);
}

inline uint64_t read_be64(const uint8_t* ptr) noexcept {
    return (static_cast<uint64_t>(read_be32(ptr)) << 32U) |
           static_cast<uint64_t>(read_be32(ptr + 4U));
}

constexpr size_t align_up_4(size_t val) noexcept {
    return (val + 3U) & ~size_t{3U};
}

bool str_eq(const char* lhs, const char* rhs) noexcept {
    size_t idx = 0U;
    while (lhs[idx] != '\0' && rhs[idx] != '\0') {
        if (lhs[idx] != rhs[idx]) {
            return false;
        }
        ++idx;
    }
    return lhs[idx] == rhs[idx];
}

bool str_starts_with(const char* str, const char* prefix) noexcept {
    size_t idx = 0U;
    while (prefix[idx] != '\0') {
        if (str[idx] != prefix[idx]) {
            return false;
        }
        ++idx;
    }
    return true;
}

bool prop_str_eq(const uint8_t* data, uint32_t len, const char* target) noexcept {
    if (data == nullptr || len == 0U) {
        return false;
    }
    size_t idx = 0U;
    while (idx < len && data[idx] != 0U && target[idx] != '\0') {
        if (static_cast<char>(data[idx]) != target[idx]) {
            return false;
        }
        ++idx;
    }
    return target[idx] == '\0' && (idx == len || data[idx] == 0U);
}

bool prop_strlist_contains(const uint8_t* data, uint32_t len, const char* target) noexcept {
    if (data == nullptr || len == 0U) {
        return false;
    }
    size_t pos = 0U;
    while (pos < len) {
        size_t end = pos;
        while (end < len && data[end] != 0U) {
            ++end;
        }
        if (end > pos && prop_str_eq(data + pos, static_cast<uint32_t>(end - pos), target)) {
            return true;
        }
        pos = end + 1U;
    }
    return false;
}

bool decode_first_reg_tuple(const uint8_t* reg_data, uint32_t reg_len, uint32_t address_cells,
                            uint32_t size_cells, uintptr_t& out_base, size_t& out_size) noexcept {
    if (reg_data == nullptr || (address_cells != 1U && address_cells != 2U) ||
        (size_cells != 1U && size_cells != 2U)) {
        return false;
    }
    const uint32_t addr_bytes = address_cells * 4U;
    const uint32_t size_bytes = size_cells * 4U;
    if (reg_len < addr_bytes + size_bytes) {
        return false;
    }

    const uint64_t raw_base =
        (address_cells == 2U) ? read_be64(reg_data) : static_cast<uint64_t>(read_be32(reg_data));
    const uint64_t raw_size = (size_cells == 2U)
                                  ? read_be64(reg_data + addr_bytes)
                                  : static_cast<uint64_t>(read_be32(reg_data + addr_bytes));
    if (raw_size == 0ULL) {
        return false;
    }

    out_base = static_cast<uintptr_t>(raw_base);
    out_size = static_cast<size_t>(raw_size);
    return true;
}

void decode_gic_interrupt_spec(const uint8_t* irq_data, uint32_t irq_len, uint32_t& out_intid,
                               uint32_t& out_trigger, bool& out_has_irq) noexcept {
    out_intid = 0U;
    out_trigger = 0U;
    out_has_irq = false;

    if (irq_data == nullptr) {
        return;
    }

    constexpr uint32_t GIC_SPI_TYPE = 0U;
    constexpr uint32_t GIC_PPI_TYPE = 1U;
    constexpr uint32_t GIC_SPI_BASE = 32U;
    constexpr uint32_t GIC_PPI_BASE = 16U;
    constexpr uint32_t GIC_MAX_INTID = 1020U;

    if (irq_len >= 12U) {
        const uint32_t irq_type = read_be32(irq_data + 0U);
        const uint32_t irq_num = read_be32(irq_data + 4U);
        const uint32_t irq_flags = read_be32(irq_data + 8U);

        if (irq_type == GIC_SPI_TYPE && irq_num <= (GIC_MAX_INTID - 1U - GIC_SPI_BASE)) {
            out_intid = irq_num + GIC_SPI_BASE;
            out_trigger = irq_flags;
            out_has_irq = true;
        } else if (irq_type == GIC_PPI_TYPE && irq_num < 16U) {
            out_intid = irq_num + GIC_PPI_BASE;
            out_trigger = irq_flags;
            out_has_irq = true;
        }
        return;
    }

    if (irq_len == 4U) {
        const uint32_t irq_num = read_be32(irq_data);
        if (irq_num <= (GIC_MAX_INTID - 1U - GIC_SPI_BASE)) {
            out_intid = irq_num + GIC_SPI_BASE;
            out_trigger = 4U;
            out_has_irq = true;
        }
    }
}

void sort_virtio_devices(Kernel::Boot::VirtioMmioList& list) noexcept {
    for (size_t idx = 1U; idx < list.count; ++idx) {
        const Kernel::Boot::VirtioMmioDevice key = list.devices[idx];
        size_t pos = idx;
        while (pos > 0U && list.devices[pos - 1U].base_paddr > key.base_paddr) {
            list.devices[pos] = list.devices[pos - 1U];
            --pos;
        }
        list.devices[pos] = key;
    }
}

struct FdtHeader {
    uint32_t magic{0U};
    uint32_t totalsize{0U};
    uint32_t off_dt_struct{0U};
    uint32_t off_dt_strings{0U};
    uint32_t off_mem_rsvmap{0U};
    uint32_t version{0U};
    uint32_t last_comp_version{0U};
    uint32_t boot_cpuid_phys{0U};
    uint32_t size_dt_strings{0U};
    uint32_t size_dt_struct{0U};
};

bool parse_header(uintptr_t dtb_paddr, FdtHeader& hdr, bool log_errors) noexcept {
    if (dtb_paddr == 0U) {
        if (log_errors) {
            fdt_print("[FDT ERROR] Invalid DTB magic!\r\n");
        }
        return false;
    }

    const auto* const base = reinterpret_cast<const uint8_t*>(dtb_paddr);
    hdr.magic = read_be32(base + 0U);
    if (hdr.magic != FDT_MAGIC) {
        if (log_errors) {
            fdt_print("[FDT ERROR] Invalid DTB magic!\r\n");
        }
        return false;
    }

    hdr.totalsize = read_be32(base + 4U);
    hdr.off_dt_struct = read_be32(base + 8U);
    hdr.off_dt_strings = read_be32(base + 12U);
    hdr.off_mem_rsvmap = read_be32(base + 16U);
    hdr.version = read_be32(base + 20U);
    hdr.last_comp_version = read_be32(base + 24U);
    hdr.boot_cpuid_phys = read_be32(base + 28U);
    hdr.size_dt_strings = read_be32(base + 32U);
    hdr.size_dt_struct = read_be32(base + 36U);

    if (hdr.totalsize < FDT_HEADER_SIZE || hdr.totalsize > FDT_MAX_SANE_SIZE) {
        return false;
    }

    if (hdr.off_dt_struct < FDT_HEADER_SIZE || hdr.off_dt_struct > hdr.totalsize ||
        hdr.size_dt_struct > hdr.totalsize - hdr.off_dt_struct) {
        return false;
    }

    if (hdr.off_dt_strings < FDT_HEADER_SIZE || hdr.off_dt_strings > hdr.totalsize ||
        hdr.size_dt_strings > hdr.totalsize - hdr.off_dt_strings) {
        return false;
    }

    if (hdr.off_mem_rsvmap < FDT_HEADER_SIZE || hdr.off_mem_rsvmap > hdr.totalsize) {
        return false;
    }

    return true;
}

const char* lookup_string(const uint8_t* strings_base, uint32_t strings_size,
                          uint32_t nameoff) noexcept {
    if (nameoff >= strings_size) {
        return nullptr;
    }
    for (uint32_t idx = nameoff; idx < strings_size; ++idx) {
        if (strings_base[idx] == 0U) {
            return reinterpret_cast<const char*>(strings_base + nameoff);
        }
    }
    return nullptr;
}

void add_aligned_region(Kernel::Boot::MemoryMap& map, uint64_t raw_base,
                        uint64_t raw_size) noexcept {
    if (raw_size == 0ULL || map.count >= Kernel::Boot::MemoryMap::MAX_REGIONS) {
        return;
    }

    const uint64_t page_mask = static_cast<uint64_t>(Arch::AArch64::PAGE_SIZE) - 1ULL;
    if (raw_base > UINT64_MAX - page_mask) {
        return;
    }

    const uint64_t raw_end =
        (UINT64_MAX - raw_base < raw_size) ? UINT64_MAX : (raw_base + raw_size);
    const uint64_t aligned_start = (raw_base + page_mask) & ~page_mask;
    const uint64_t aligned_end = raw_end & ~page_mask;

    if (aligned_end <= aligned_start) {
        return;
    }

    map.regions[map.count].base_paddr = static_cast<uintptr_t>(aligned_start);
    map.regions[map.count].size = static_cast<size_t>(aligned_end - aligned_start);
    ++map.count;
}

void decode_reg_property(Kernel::Boot::MemoryMap& map, const uint8_t* reg_data, uint32_t reg_len,
                         uint32_t address_cells, uint32_t size_cells) noexcept {
    if (reg_data == nullptr || (address_cells != 1U && address_cells != 2U) ||
        (size_cells != 1U && size_cells != 2U)) {
        return;
    }

    const uint32_t addr_bytes = address_cells * 4U;
    const uint32_t size_bytes = size_cells * 4U;
    const uint32_t tuple_bytes = addr_bytes + size_bytes;

    for (uint32_t offset = 0U; offset + tuple_bytes <= reg_len; offset += tuple_bytes) {
        const uint64_t base =
            (address_cells == 2U) ? read_be64(reg_data + offset) : read_be32(reg_data + offset);
        const uint64_t size = (size_cells == 2U) ? read_be64(reg_data + offset + addr_bytes)
                                                 : read_be32(reg_data + offset + addr_bytes);
        add_aligned_region(map, base, size);
    }
}

void sort_and_merge_regions(Kernel::Boot::MemoryMap& map) noexcept {
    if (map.count == 0U) {
        return;
    }

    // Sort regions in ascending order of physical base address
    for (size_t i = 1U; i < map.count; ++i) {
        const Kernel::Boot::MemoryRegion key = map.regions[i];
        size_t j = i;
        while (j > 0U && map.regions[j - 1U].base_paddr > key.base_paddr) {
            map.regions[j] = map.regions[j - 1U];
            --j;
        }
        map.regions[j] = key;
    }

    // Merge adjacent or overlapping spans
    size_t write_idx = 0U;
    for (size_t read_idx = 1U; read_idx < map.count; ++read_idx) {
        Kernel::Boot::MemoryRegion& curr = map.regions[write_idx];
        const Kernel::Boot::MemoryRegion& next = map.regions[read_idx];
        const uintptr_t curr_end = curr.base_paddr + curr.size;

        if (next.base_paddr <= curr_end) {
            const uintptr_t next_end = next.base_paddr + next.size;
            if (next_end > curr_end) {
                curr.size = static_cast<size_t>(next_end - curr.base_paddr);
            }
        } else {
            ++write_idx;
            map.regions[write_idx] = next;
        }
    }
    map.count = write_idx + 1U;
}

void finalize_memory_map(Kernel::Boot::MemoryMap& map) noexcept {
    if (map.count == 0U) {
        map.regions[0] = Kernel::Boot::MemoryRegion{.base_paddr = Arch::AArch64::RAM_BASE,
                                                    .size = FALLBACK_RAM_SIZE};
        map.count = 1U;
    }

    map.min_paddr = map.regions[0].base_paddr;
    const Kernel::Boot::MemoryRegion& last = map.regions[map.count - 1U];
    map.max_paddr = last.base_paddr + last.size;

    size_t total = 0U;
    for (size_t idx = 0U; idx < map.count; ++idx) {
        total += map.regions[idx].size;
    }
    map.total_usable_bytes = total;
}

} // namespace

namespace Kernel::Boot {

bool validate_header(uintptr_t dtb_paddr, size_t* out_totalsize) noexcept {
    FdtHeader hdr{};
    if (!parse_header(dtb_paddr, hdr, true)) {
        return false;
    }
    if (out_totalsize != nullptr) {
        *out_totalsize = static_cast<size_t>(hdr.totalsize);
    }
    return true;
}

bool find_memory_regions(uintptr_t dtb_paddr, MemoryMap& out_map) noexcept {
    out_map = MemoryMap{};

    FdtHeader hdr{};
    if (!parse_header(dtb_paddr, hdr, true)) {
        return false;
    }

    const auto* const base = reinterpret_cast<const uint8_t*>(dtb_paddr);
    const auto* const struct_base = base + hdr.off_dt_struct;
    const auto* const strings_base = base + hdr.off_dt_strings;
    const uint32_t struct_size = hdr.size_dt_struct;
    const uint32_t strings_size = hdr.size_dt_strings;

    uint32_t address_cells = 2U;
    uint32_t size_cells = 2U;
    uint32_t depth = 0U;

    bool node_is_memory_name = false;
    bool node_has_device_type_memory = false;
    bool node_is_disabled = false;
    const uint8_t* node_reg_data = nullptr;
    uint32_t node_reg_len = 0U;

    uint32_t offset = 0U;
    while (offset + 4U <= struct_size) {
        const uint32_t token = read_be32(struct_base + offset);
        offset += 4U;

        if (token == FDT_NOP) {
            continue;
        }

        if (token == FDT_END) {
            break;
        }

        if (token == FDT_BEGIN_NODE) {
            uint32_t name_end = offset;
            while (name_end < struct_size && struct_base[name_end] != 0U) {
                ++name_end;
            }
            if (name_end >= struct_size) {
                break;
            }

            const auto* const node_name = reinterpret_cast<const char*>(struct_base + offset);
            const size_t name_with_nul = static_cast<size_t>(name_end - offset) + 1U;
            const size_t padded_len = align_up_4(name_with_nul);
            if (padded_len > struct_size - offset) {
                break;
            }
            offset += static_cast<uint32_t>(padded_len);
            ++depth;

            if (depth == 2U) {
                node_is_memory_name =
                    str_eq(node_name, "memory") || str_starts_with(node_name, "memory@");
                node_has_device_type_memory = false;
                node_is_disabled = false;
                node_reg_data = nullptr;
                node_reg_len = 0U;
            }
            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth == 2U) {
                if ((node_is_memory_name || node_has_device_type_memory) && !node_is_disabled &&
                    node_reg_data != nullptr && node_reg_len > 0U) {
                    decode_reg_property(out_map, node_reg_data, node_reg_len, address_cells,
                                        size_cells);
                }
                node_is_memory_name = false;
                node_has_device_type_memory = false;
                node_is_disabled = false;
                node_reg_data = nullptr;
                node_reg_len = 0U;
            }
            if (depth > 0U) {
                --depth;
            }
            continue;
        }

        if (token == FDT_PROP) {
            if (offset + 8U > struct_size) {
                break;
            }
            const uint32_t prop_len = read_be32(struct_base + offset);
            const uint32_t nameoff = read_be32(struct_base + offset + 4U);
            offset += 8U;

            const size_t padded_len = align_up_4(prop_len);
            if (padded_len > struct_size - offset) {
                break;
            }

            const auto* const prop_data = struct_base + offset;
            offset += static_cast<uint32_t>(padded_len);

            const char* const prop_name = lookup_string(strings_base, strings_size, nameoff);
            if (prop_name == nullptr) {
                continue;
            }

            if (depth == 1U) {
                if (str_eq(prop_name, "#address-cells") && prop_len == 4U) {
                    address_cells = read_be32(prop_data);
                } else if (str_eq(prop_name, "#size-cells") && prop_len == 4U) {
                    size_cells = read_be32(prop_data);
                }
            } else if (depth == 2U) {
                if (str_eq(prop_name, "reg")) {
                    node_reg_data = prop_data;
                    node_reg_len = prop_len;
                } else if (str_eq(prop_name, "device_type")) {
                    node_has_device_type_memory = prop_str_eq(prop_data, prop_len, "memory");
                } else if (str_eq(prop_name, "status")) {
                    node_is_disabled = prop_str_eq(prop_data, prop_len, "disabled") ||
                                       prop_str_eq(prop_data, prop_len, "fail");
                }
            }
            continue;
        }

        // Unknown token
        break;
    }

    sort_and_merge_regions(out_map);
    finalize_memory_map(out_map);
    return true;
}

MemoryMap find_memory_regions(uintptr_t dtb_paddr) noexcept {
    MemoryMap map{};
    if (!find_memory_regions(dtb_paddr, map)) {
        finalize_memory_map(map);
    }
    return map;
}

ReserveMap find_memory_reservations(uintptr_t dtb_paddr) noexcept {
    ReserveMap rsv{};

    FdtHeader hdr{};
    if (!parse_header(dtb_paddr, hdr, false)) {
        return rsv;
    }

    const auto* const base = reinterpret_cast<const uint8_t*>(dtb_paddr);
    uint32_t offset = hdr.off_mem_rsvmap;

    while (offset + 16U <= hdr.totalsize && rsv.count < ReserveMap::MAX_ENTRIES) {
        const uint64_t addr = read_be64(base + offset);
        const uint64_t size = read_be64(base + offset + 8U);
        offset += 16U;

        if (addr == 0ULL && size == 0ULL) {
            break;
        }
        if (size > 0ULL) {
            rsv.entries[rsv.count] = MemoryRegion{
                .base_paddr = static_cast<uintptr_t>(addr),
                .size = static_cast<size_t>(size),
            };
            ++rsv.count;
        }
    }

    return rsv;
}

InitrdLocation find_initrd(uintptr_t dtb_paddr) noexcept {
    InitrdLocation info{};

    FdtHeader hdr{};
    if (!parse_header(dtb_paddr, hdr, false)) {
        return info;
    }

    const auto* const base = reinterpret_cast<const uint8_t*>(dtb_paddr);
    const auto* const struct_base = base + hdr.off_dt_struct;
    const auto* const strings_base = base + hdr.off_dt_strings;
    const uint32_t struct_size = hdr.size_dt_struct;
    const uint32_t strings_size = hdr.size_dt_strings;

    uint32_t depth = 0U;
    bool in_chosen = false;
    bool has_start = false;
    bool has_end = false;
    uint64_t start_val = 0ULL;
    uint64_t end_val = 0ULL;

    uint32_t offset = 0U;
    while (offset + 4U <= struct_size) {
        const uint32_t token = read_be32(struct_base + offset);
        offset += 4U;

        if (token == FDT_NOP) {
            continue;
        }
        if (token == FDT_END) {
            break;
        }
        if (token == FDT_BEGIN_NODE) {
            uint32_t name_end = offset;
            while (name_end < struct_size && struct_base[name_end] != 0U) {
                ++name_end;
            }
            if (name_end >= struct_size) {
                break;
            }
            const auto* const node_name = reinterpret_cast<const char*>(struct_base + offset);
            const size_t padded_len = align_up_4(static_cast<size_t>(name_end - offset) + 1U);
            if (padded_len > struct_size - offset) {
                break;
            }
            offset += static_cast<uint32_t>(padded_len);
            ++depth;
            if (depth == 2U) {
                in_chosen = str_eq(node_name, "chosen") || str_starts_with(node_name, "chosen@");
            }
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == 2U) {
                in_chosen = false;
            }
            if (depth > 0U) {
                --depth;
            }
            continue;
        }
        if (token == FDT_PROP) {
            if (offset + 8U > struct_size) {
                break;
            }
            const uint32_t prop_len = read_be32(struct_base + offset);
            const uint32_t nameoff = read_be32(struct_base + offset + 4U);
            offset += 8U;

            const size_t padded_len = align_up_4(prop_len);
            if (padded_len > struct_size - offset) {
                break;
            }
            const auto* const prop_data = struct_base + offset;
            offset += static_cast<uint32_t>(padded_len);

            if (depth == 2U && in_chosen) {
                const char* const prop_name = lookup_string(strings_base, strings_size, nameoff);
                if (prop_name == nullptr) {
                    continue;
                }
                if (str_eq(prop_name, "linux,initrd-start")) {
                    if (prop_len == 4U) {
                        start_val = read_be32(prop_data);
                        has_start = true;
                    } else if (prop_len == 8U) {
                        start_val = read_be64(prop_data);
                        has_start = true;
                    }
                } else if (str_eq(prop_name, "linux,initrd-end")) {
                    if (prop_len == 4U) {
                        end_val = read_be32(prop_data);
                        has_end = true;
                    } else if (prop_len == 8U) {
                        end_val = read_be64(prop_data);
                        has_end = true;
                    }
                }
            }
            continue;
        }
        break;
    }

    if (has_start && has_end && end_val > start_val) {
        info.start_paddr = static_cast<uintptr_t>(start_val);
        info.end_paddr = static_cast<uintptr_t>(end_val);
        info.size = static_cast<size_t>(end_val - start_val);
        info.valid = true;
    }

    return info;
}

bool find_virtio_mmio_devices(uintptr_t dtb_paddr, VirtioMmioList& out_list) noexcept {
    out_list = VirtioMmioList{};

    FdtHeader hdr{};
    if (!parse_header(dtb_paddr, hdr, false)) {
        return false;
    }

    const auto* const base = reinterpret_cast<const uint8_t*>(dtb_paddr);
    const auto* const struct_base = base + hdr.off_dt_struct;
    const auto* const strings_base = base + hdr.off_dt_strings;
    const uint32_t struct_size = hdr.size_dt_struct;
    const uint32_t strings_size = hdr.size_dt_strings;

    constexpr size_t MAX_DTB_DEPTH = 16U;
    struct NodeFrame {
        const uint8_t* reg_data{nullptr};
        const uint8_t* irq_data{nullptr};
        uint32_t addr_cells{2U};
        uint32_t size_cells{2U};
        uint32_t reg_len{0U};
        uint32_t irq_len{0U};
        bool is_virtio_mmio{false};
        bool is_disabled{false};
    };

    NodeFrame stack[MAX_DTB_DEPTH]{};
    uint32_t depth = 0U;

    uint32_t offset = 0U;
    while (offset + 4U <= struct_size) {
        const uint32_t token = read_be32(struct_base + offset);
        offset += 4U;

        if (token == FDT_NOP) {
            continue;
        }
        if (token == FDT_END) {
            break;
        }
        if (token == FDT_BEGIN_NODE) {
            uint32_t name_end = offset;
            while (name_end < struct_size && struct_base[name_end] != 0U) {
                ++name_end;
            }
            if (name_end >= struct_size) {
                break;
            }
            const size_t padded_len = align_up_4(static_cast<size_t>(name_end - offset) + 1U);
            if (padded_len > struct_size - offset) {
                break;
            }
            offset += static_cast<uint32_t>(padded_len);

            if (depth < MAX_DTB_DEPTH) {
                const uint32_t parent_ac = (depth > 0U) ? stack[depth - 1U].addr_cells : 2U;
                const uint32_t parent_sc = (depth > 0U) ? stack[depth - 1U].size_cells : 2U;
                stack[depth] = NodeFrame{
                    .addr_cells = parent_ac,
                    .size_cells = parent_sc,
                };
            }
            ++depth;
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth > 0U) {
                --depth;
                if (depth < MAX_DTB_DEPTH) {
                    const NodeFrame& frame = stack[depth];
                    if (frame.is_virtio_mmio && !frame.is_disabled && frame.reg_data != nullptr &&
                        out_list.count < VirtioMmioList::MAX_DEVICES) {
                        const uint32_t parent_ac = (depth > 0U) ? stack[depth - 1U].addr_cells : 2U;
                        const uint32_t parent_sc = (depth > 0U) ? stack[depth - 1U].size_cells : 2U;

                        uintptr_t dev_base = 0U;
                        size_t dev_size = 0U;
                        if (decode_first_reg_tuple(frame.reg_data, frame.reg_len, parent_ac,
                                                   parent_sc, dev_base, dev_size)) {
                            VirtioMmioDevice dev{};
                            dev.base_paddr = dev_base;
                            dev.size = dev_size;
                            decode_gic_interrupt_spec(frame.irq_data, frame.irq_len, dev.irq_intid,
                                                      dev.irq_trigger, dev.has_irq);
                            out_list.devices[out_list.count++] = dev;
                        }
                    }
                }
            }
            continue;
        }
        if (token == FDT_PROP) {
            if (offset + 8U > struct_size) {
                break;
            }
            const uint32_t prop_len = read_be32(struct_base + offset);
            const uint32_t nameoff = read_be32(struct_base + offset + 4U);
            offset += 8U;

            const size_t padded_len = align_up_4(prop_len);
            if (padded_len > struct_size - offset) {
                break;
            }
            const auto* const prop_data = struct_base + offset;
            offset += static_cast<uint32_t>(padded_len);

            if (depth == 0U || depth > MAX_DTB_DEPTH) {
                continue;
            }

            const char* const prop_name = lookup_string(strings_base, strings_size, nameoff);
            if (prop_name == nullptr) {
                continue;
            }

            NodeFrame& frame = stack[depth - 1U];
            if (str_eq(prop_name, "#address-cells") && prop_len == 4U) {
                frame.addr_cells = read_be32(prop_data);
            } else if (str_eq(prop_name, "#size-cells") && prop_len == 4U) {
                frame.size_cells = read_be32(prop_data);
            } else if (str_eq(prop_name, "compatible")) {
                if (prop_strlist_contains(prop_data, prop_len, "virtio,mmio")) {
                    frame.is_virtio_mmio = true;
                }
            } else if (str_eq(prop_name, "status")) {
                frame.is_disabled = prop_str_eq(prop_data, prop_len, "disabled") ||
                                    prop_str_eq(prop_data, prop_len, "fail");
            } else if (str_eq(prop_name, "reg")) {
                frame.reg_data = prop_data;
                frame.reg_len = prop_len;
            } else if (str_eq(prop_name, "interrupts")) {
                frame.irq_data = prop_data;
                frame.irq_len = prop_len;
            }
            continue;
        }
        break;
    }

    sort_virtio_devices(out_list);
    return true;
}

VirtioMmioList find_virtio_mmio_devices(uintptr_t dtb_paddr) noexcept {
    VirtioMmioList list{};
    (void)find_virtio_mmio_devices(dtb_paddr, list);
    return list;
}

} // namespace Kernel::Boot