#include "boot/fdt.hpp"
#include "memory_config.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

class DtbBuilder {
  public:
    void add_memreserve(uint64_t addr, uint64_t size) {
        append_be64(rsvmap_, addr);
        append_be64(rsvmap_, size);
    }

    void begin_node(const char* name) {
        append_be32(struct_blk_, 0x00000001U);
        const size_t len = std::strlen(name) + 1U;
        struct_blk_.insert(struct_blk_.end(), reinterpret_cast<const uint8_t*>(name),
                           reinterpret_cast<const uint8_t*>(name) + len);
        while ((struct_blk_.size() % 4U) != 0U) {
            struct_blk_.push_back(0U);
        }
    }

    void end_node() { append_be32(struct_blk_, 0x00000002U); }

    void add_prop_u32(const char* name, uint32_t val) {
        std::vector<uint8_t> data;
        append_be32(data, val);
        add_prop_raw(name, data.data(), data.size());
    }

    void add_prop_str(const char* name, const char* val) {
        add_prop_raw(name, reinterpret_cast<const uint8_t*>(val), std::strlen(val) + 1U);
    }

    void add_prop_raw(const char* name, const uint8_t* data, size_t len) {
        const uint32_t nameoff = add_string(name);
        append_be32(struct_blk_, 0x00000003U);
        append_be32(struct_blk_, static_cast<uint32_t>(len));
        append_be32(struct_blk_, nameoff);
        if (len > 0U) {
            struct_blk_.insert(struct_blk_.end(), data, data + len);
        }
        while ((struct_blk_.size() % 4U) != 0U) {
            struct_blk_.push_back(0U);
        }
    }

    std::vector<uint8_t> finalize() const {
        std::vector<uint8_t> rsv = rsvmap_;
        append_be64(rsv, 0ULL);
        append_be64(rsv, 0ULL);

        std::vector<uint8_t> st = struct_blk_;
        append_be32(st, 0x00000009U); // FDT_END

        const uint32_t off_rsv = 40U;
        const uint32_t off_struct = off_rsv + static_cast<uint32_t>(rsv.size());
        const uint32_t off_strings = off_struct + static_cast<uint32_t>(st.size());
        const uint32_t totalsize = off_strings + static_cast<uint32_t>(strings_blk_.size());

        std::vector<uint8_t> blob;
        blob.reserve(totalsize);
        append_be32(blob, 0xD00DFEEDU);
        append_be32(blob, totalsize);
        append_be32(blob, off_struct);
        append_be32(blob, off_strings);
        append_be32(blob, off_rsv);
        append_be32(blob, 17U);
        append_be32(blob, 16U);
        append_be32(blob, 0U);
        append_be32(blob, static_cast<uint32_t>(strings_blk_.size()));
        append_be32(blob, static_cast<uint32_t>(st.size()));

        blob.insert(blob.end(), rsv.begin(), rsv.end());
        blob.insert(blob.end(), st.begin(), st.end());
        blob.insert(blob.end(), strings_blk_.begin(), strings_blk_.end());
        return blob;
    }

    static void append_be32(std::vector<uint8_t>& buf, uint32_t val) {
        buf.push_back(static_cast<uint8_t>((val >> 24U) & 0xFFU));
        buf.push_back(static_cast<uint8_t>((val >> 16U) & 0xFFU));
        buf.push_back(static_cast<uint8_t>((val >> 8U) & 0xFFU));
        buf.push_back(static_cast<uint8_t>(val & 0xFFU));
    }

    static void append_be64(std::vector<uint8_t>& buf, uint64_t val) {
        append_be32(buf, static_cast<uint32_t>((val >> 32U) & 0xFFFFFFFFULL));
        append_be32(buf, static_cast<uint32_t>(val & 0xFFFFFFFFULL));
    }

  private:
    uint32_t add_string(const char* name) {
        const auto off = static_cast<uint32_t>(strings_blk_.size());
        const size_t len = std::strlen(name) + 1U;
        strings_blk_.insert(strings_blk_.end(), reinterpret_cast<const uint8_t*>(name),
                            reinterpret_cast<const uint8_t*>(name) + len);
        return off;
    }

    std::vector<uint8_t> rsvmap_;
    std::vector<uint8_t> struct_blk_;
    std::vector<uint8_t> strings_blk_;
};

} // namespace

TEST_CASE("FDT", "Single-Bank 1 GiB Memory Node Parsing") {
    DtbBuilder b;
    b.begin_node("");
    b.add_prop_u32("#address-cells", 2U);
    b.add_prop_u32("#size-cells", 2U);

    b.begin_node("memory@40000000");
    b.add_prop_str("device_type", "memory");
    std::vector<uint8_t> reg;
    DtbBuilder::append_be64(reg, 0x40000000ULL);
    DtbBuilder::append_be64(reg, 0x40000000ULL); // 1 GiB
    b.add_prop_raw("reg", reg.data(), reg.size());
    b.end_node();

    b.end_node();

    const auto blob = b.finalize();
    const FDT::MemoryMap map = FDT::find_memory_regions(reinterpret_cast<uintptr_t>(blob.data()));
    EXPECT_EQ(map.count, 1U);
    EXPECT_EQ(map.min_paddr, static_cast<uintptr_t>(0x40000000ULL));
    EXPECT_EQ(map.max_paddr, static_cast<uintptr_t>(0x80000000ULL));
    EXPECT_EQ(map.total_usable_bytes, static_cast<size_t>(0x40000000ULL));
}

TEST_CASE("FDT", "Multi-Bank Discontiguous Regions, Sorting & Merging") {
    DtbBuilder b;
    b.begin_node("");
    b.add_prop_u32("#address-cells", 2U);
    b.add_prop_u32("#size-cells", 2U);

    b.begin_node("memory@100000000");
    std::vector<uint8_t> reg2;
    DtbBuilder::append_be64(reg2, 0x100000000ULL);
    DtbBuilder::append_be64(reg2, 0x80000000ULL); // 2 GiB @ 4 GiB
    b.add_prop_raw("reg", reg2.data(), reg2.size());
    b.end_node();

    b.begin_node("memory@40000000");
    std::vector<uint8_t> reg1;
    DtbBuilder::append_be64(reg1, 0x40000000ULL);
    DtbBuilder::append_be64(reg1, 0x20000000ULL);
    DtbBuilder::append_be64(reg1, 0x60000000ULL);
    DtbBuilder::append_be64(reg1, 0x20000000ULL);
    b.add_prop_raw("reg", reg1.data(), reg1.size());
    b.end_node();

    b.end_node();

    const auto blob = b.finalize();
    const FDT::MemoryMap map = FDT::find_memory_regions(reinterpret_cast<uintptr_t>(blob.data()));
    EXPECT_EQ(map.count, 2U);
    EXPECT_EQ(map.regions[0].base_paddr, static_cast<uintptr_t>(0x40000000ULL));
    EXPECT_EQ(map.regions[0].size, static_cast<size_t>(0x40000000ULL));
    EXPECT_EQ(map.regions[1].base_paddr, static_cast<uintptr_t>(0x100000000ULL));
    EXPECT_EQ(map.regions[1].size, static_cast<size_t>(0x80000000ULL));
    EXPECT_EQ(map.min_paddr, static_cast<uintptr_t>(0x40000000ULL));
    EXPECT_EQ(map.max_paddr, static_cast<uintptr_t>(0x180000000ULL));
    EXPECT_EQ(map.total_usable_bytes, static_cast<size_t>(0xC0000000ULL));
}

TEST_CASE("FDT", "1-Cell Encoding, Page Alignment, /memreserve/ & /chosen Initrd") {
    DtbBuilder b;
    b.add_memreserve(0x40000000ULL, 0x10000ULL);
    b.add_memreserve(0x47F00000ULL, 0x100000ULL);

    b.begin_node("");
    b.add_prop_u32("#address-cells", 1U);
    b.add_prop_u32("#size-cells", 1U);

    b.begin_node("memory");
    std::vector<uint8_t> reg;
    DtbBuilder::append_be32(reg, 0x40000100U); // Aligns up to 0x40001000
    DtbBuilder::append_be32(reg, 0x10000000U); // End 0x50000100 aligns down to 0x50000000
    b.add_prop_raw("reg", reg.data(), reg.size());
    b.end_node();

    b.begin_node("chosen");
    b.add_prop_u32("linux,initrd-start", 0x44000000U);
    b.add_prop_u32("linux,initrd-end", 0x44250000U);
    b.end_node();

    b.end_node();

    const auto blob = b.finalize();
    const auto dtb_addr = reinterpret_cast<uintptr_t>(blob.data());

    const FDT::MemoryMap map = FDT::find_memory_regions(dtb_addr);
    EXPECT_EQ(map.count, 1U);
    EXPECT_EQ(map.regions[0].base_paddr, static_cast<uintptr_t>(0x40001000ULL));
    EXPECT_EQ(map.regions[0].size, static_cast<size_t>(0x50000000ULL - 0x40001000ULL));

    const FDT::ReserveMap rsv = FDT::find_memory_reservations(dtb_addr);
    EXPECT_EQ(rsv.count, 2U);
    EXPECT_EQ(rsv.entries[0].base_paddr, static_cast<uintptr_t>(0x40000000ULL));
    EXPECT_EQ(rsv.entries[0].size, static_cast<size_t>(0x10000ULL));
    EXPECT_EQ(rsv.entries[1].base_paddr, static_cast<uintptr_t>(0x47F00000ULL));
    EXPECT_EQ(rsv.entries[1].size, static_cast<size_t>(0x100000ULL));

    const FDT::InitrdLocation initrd = FDT::find_initrd(dtb_addr);
    EXPECT_TRUE(initrd.valid);
    EXPECT_EQ(initrd.start_paddr, static_cast<uintptr_t>(0x44000000ULL));
    EXPECT_EQ(initrd.end_paddr, static_cast<uintptr_t>(0x44250000ULL));
    EXPECT_EQ(initrd.size, static_cast<size_t>(0x250000ULL));
}

TEST_CASE("FDT", "Missing /memory Fallback & Corrupted Header Rejection") {
    DtbBuilder b;
    b.begin_node("");
    b.begin_node("chosen");
    b.end_node();
    b.end_node();

    const auto blob = b.finalize();
    const FDT::MemoryMap fallback =
        FDT::find_memory_regions(reinterpret_cast<uintptr_t>(blob.data()));
    EXPECT_EQ(fallback.count, 1U);
    EXPECT_EQ(fallback.min_paddr, Arch::RAM_BASE);
    EXPECT_EQ(fallback.max_paddr, Arch::RAM_BASE + (256UL * 1024UL * 1024UL));

    std::vector<uint8_t> bad = blob;
    bad[0] = 0xDE;
    bad[1] = 0xAD;
    bad[2] = 0xBE;
    bad[3] = 0xEF;
    FDT::MemoryMap out{};
    EXPECT_FALSE(FDT::validate_header(reinterpret_cast<uintptr_t>(bad.data())));
    EXPECT_FALSE(FDT::find_memory_regions(reinterpret_cast<uintptr_t>(bad.data()), out));
}