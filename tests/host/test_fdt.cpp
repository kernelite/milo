#include "boot/fdt.hpp"
#include "test_framework.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t TOKEN_BEGIN_NODE = 0x00000001U;
constexpr uint32_t TOKEN_END_NODE = 0x00000002U;
constexpr uint32_t TOKEN_PROP = 0x00000003U;
constexpr uint32_t TOKEN_NOP = 0x00000004U;
constexpr uint32_t TOKEN_END = 0x00000009U;

void push_be32(std::vector<uint8_t>& out, uint32_t val) {
    out.push_back(static_cast<uint8_t>((val >> 24U) & 0xFFU));
    out.push_back(static_cast<uint8_t>((val >> 16U) & 0xFFU));
    out.push_back(static_cast<uint8_t>((val >> 8U) & 0xFFU));
    out.push_back(static_cast<uint8_t>(val & 0xFFU));
}

void push_be64(std::vector<uint8_t>& out, uint64_t val) {
    push_be32(out, static_cast<uint32_t>(val >> 32U));
    push_be32(out, static_cast<uint32_t>(val & 0xFFFFFFFFULL));
}

uint32_t add_string(std::vector<uint8_t>& strings, const std::string& str) {
    const auto offset = static_cast<uint32_t>(strings.size());
    strings.insert(strings.end(), str.begin(), str.end());
    strings.push_back(0U);
    return offset;
}

void begin_node(std::vector<uint8_t>& dt_struct, const std::string& name) {
    push_be32(dt_struct, TOKEN_BEGIN_NODE);
    dt_struct.insert(dt_struct.end(), name.begin(), name.end());
    dt_struct.push_back(0U);
    while ((dt_struct.size() % 4U) != 0U) {
        dt_struct.push_back(0U);
    }
}

void end_node(std::vector<uint8_t>& dt_struct) {
    push_be32(dt_struct, TOKEN_END_NODE);
}

void add_prop_u32(std::vector<uint8_t>& dt_struct, std::vector<uint8_t>& strings,
                  const std::string& name, uint32_t val) {
    push_be32(dt_struct, TOKEN_PROP);
    push_be32(dt_struct, 4U);
    push_be32(dt_struct, add_string(strings, name));
    push_be32(dt_struct, val);
}

void add_prop_u64(std::vector<uint8_t>& dt_struct, std::vector<uint8_t>& strings,
                  const std::string& name, uint64_t val) {
    push_be32(dt_struct, TOKEN_PROP);
    push_be32(dt_struct, 8U);
    push_be32(dt_struct, add_string(strings, name));
    push_be64(dt_struct, val);
}

struct AlignedDtb {
    alignas(8) std::vector<uint8_t> storage;
    uintptr_t paddr() const noexcept { return reinterpret_cast<uintptr_t>(storage.data()); }
};

AlignedDtb build_test_dtb(bool include_chosen, bool include_start, bool include_end,
                          bool use_64bit_addrs, uint64_t initrd_start, uint64_t initrd_end) {
    std::vector<uint8_t> dt_struct;
    std::vector<uint8_t> dt_strings;

    // Root node "/"
    begin_node(dt_struct, "");
    push_be32(dt_struct, TOKEN_NOP);

    // Sibling node "/memory@40000000"
    begin_node(dt_struct, "memory@40000000");
    add_prop_u32(dt_struct, dt_strings, "#address-cells", 2U);
    end_node(dt_struct);

    if (include_chosen) {
        begin_node(dt_struct, "chosen");
        if (include_start) {
            if (use_64bit_addrs) {
                add_prop_u64(dt_struct, dt_strings, "linux,initrd-start", initrd_start);
            } else {
                add_prop_u32(dt_struct, dt_strings, "linux,initrd-start",
                             static_cast<uint32_t>(initrd_start));
            }
        }
        if (include_end) {
            if (use_64bit_addrs) {
                add_prop_u64(dt_struct, dt_strings, "linux,initrd-end", initrd_end);
            } else {
                add_prop_u32(dt_struct, dt_strings, "linux,initrd-end",
                             static_cast<uint32_t>(initrd_end));
            }
        }
        end_node(dt_struct);
    }

    end_node(dt_struct); // end root
    push_be32(dt_struct, TOKEN_END);

    // Header (40 bytes) + 16-byte empty mem_rsvmap + dt_struct + dt_strings
    constexpr uint32_t header_size = sizeof(FDT::Header);
    constexpr uint32_t rsvmap_size = 16U;
    const uint32_t off_mem_rsvmap = header_size;
    const uint32_t off_dt_struct = off_mem_rsvmap + rsvmap_size;
    const auto size_dt_struct = static_cast<uint32_t>(dt_struct.size());
    const uint32_t off_dt_strings = off_dt_struct + size_dt_struct;
    const auto size_dt_strings = static_cast<uint32_t>(dt_strings.size());
    const uint32_t totalsize = off_dt_strings + size_dt_strings;

    AlignedDtb dtb{};
    dtb.storage.reserve(totalsize);
    push_be32(dtb.storage, FDT::FDT_MAGIC);
    push_be32(dtb.storage, totalsize);
    push_be32(dtb.storage, off_dt_struct);
    push_be32(dtb.storage, off_dt_strings);
    push_be32(dtb.storage, off_mem_rsvmap);
    push_be32(dtb.storage, 17U); // version
    push_be32(dtb.storage, 16U); // last_comp_version
    push_be32(dtb.storage, 0U);  // boot_cpuid_phys
    push_be32(dtb.storage, size_dt_strings);
    push_be32(dtb.storage, size_dt_struct);

    // Empty mem_rsvmap terminator (address = 0, size = 0)
    push_be64(dtb.storage, 0ULL);
    push_be64(dtb.storage, 0ULL);

    dtb.storage.insert(dtb.storage.end(), dt_struct.begin(), dt_struct.end());
    dtb.storage.insert(dtb.storage.end(), dt_strings.begin(), dt_strings.end());
    return dtb;
}

} // namespace

TEST_CASE("FDT", "Header Magic, Alignment & Total Size Validation") {
    const auto dtb = build_test_dtb(true, true, true, true, 0x44000000ULL, 0x44100000ULL);
    EXPECT_TRUE(FDT::is_valid(dtb.paddr()));
    EXPECT_EQ(FDT::get_total_size(dtb.paddr()), static_cast<uint32_t>(dtb.storage.size()));

    // Null & unaligned pointers
    EXPECT_FALSE(FDT::is_valid(0U));
    EXPECT_EQ(FDT::get_total_size(0U), 0U);
    EXPECT_FALSE(FDT::is_valid(dtb.paddr() + 1U));
    EXPECT_FALSE(FDT::is_valid(dtb.paddr() + 2U));
    EXPECT_FALSE(FDT::is_valid(dtb.paddr() + 3U));

    // Corrupted magic
    auto bad_dtb = dtb;
    bad_dtb.storage[0] = 0x00U;
    EXPECT_FALSE(FDT::is_valid(bad_dtb.paddr()));
    EXPECT_EQ(FDT::get_total_size(bad_dtb.paddr()), 0U);
}

TEST_CASE("FDT", "Big-Endian Reader Alignment Safety on Arbitrary Offsets") {
    // Exercise 64-bit property values that are 4-byte aligned (NOT 8-byte aligned) inside DTB
    // under UBSan (-fsanitize=undefined) to guarantee zero unaligned load traps.
    const auto dtb64 = build_test_dtb(true, true, true, true, 0x48123000ULL, 0x489ABC00ULL);
    const auto loc64 = FDT::find_initrd(dtb64.paddr());
    EXPECT_TRUE(loc64.valid);
    EXPECT_EQ(loc64.start_paddr, 0x48123000ULL);
    EXPECT_EQ(loc64.end_paddr, 0x489ABC00ULL);
    EXPECT_EQ(loc64.size, 0x00888C00ULL);

    // 32-bit cell properties
    const auto dtb32 = build_test_dtb(true, true, true, false, 0x42000000ULL, 0x42080000ULL);
    const auto loc32 = FDT::find_initrd(dtb32.paddr());
    EXPECT_TRUE(loc32.valid);
    EXPECT_EQ(loc32.start_paddr, 0x42000000ULL);
    EXPECT_EQ(loc32.end_paddr, 0x42080000ULL);
    EXPECT_EQ(loc32.size, 0x00080000ULL);
}

TEST_CASE("FDT", "Chosen Node Scanner Presence & Missing Property Edge Cases") {
    // /chosen missing completely
    {
        const auto dtb = build_test_dtb(false, false, false, true, 0ULL, 0ULL);
        const auto loc = FDT::find_initrd(dtb.paddr());
        EXPECT_FALSE(loc.valid);
        EXPECT_EQ(loc.size, 0U);
    }

    // /chosen present, but both properties missing
    {
        const auto dtb = build_test_dtb(true, false, false, true, 0ULL, 0ULL);
        const auto loc = FDT::find_initrd(dtb.paddr());
        EXPECT_FALSE(loc.valid);
    }

    // /chosen present with only linux,initrd-start
    {
        const auto dtb = build_test_dtb(true, true, false, true, 0x44000000ULL, 0ULL);
        const auto loc = FDT::find_initrd(dtb.paddr());
        EXPECT_FALSE(loc.valid);
    }

    // /chosen present with only linux,initrd-end
    {
        const auto dtb = build_test_dtb(true, false, true, true, 0ULL, 0x44100000ULL);
        const auto loc = FDT::find_initrd(dtb.paddr());
        EXPECT_FALSE(loc.valid);
    }

    // Inverted range (end < start)
    {
        const auto dtb = build_test_dtb(true, true, true, true, 0x45000000ULL, 0x44000000ULL);
        const auto loc = FDT::find_initrd(dtb.paddr());
        EXPECT_FALSE(loc.valid);
    }
}