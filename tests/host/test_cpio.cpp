#include "fs/cpio.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void write_hex8(char dst[8], uint32_t value) {
    char tmp[9];
    (void)std::snprintf(tmp, sizeof(tmp), "%08X", value);
    std::memcpy(dst, tmp, 8U);
}

void append_cpio_record(std::vector<uint8_t>& archive, const char* magic, uint32_t ino,
                        uint32_t mode, const std::string& name, const std::vector<uint8_t>& data) {
    const uint32_t namesize = static_cast<uint32_t>(name.size() + 1U);
    const uint32_t filesize = static_cast<uint32_t>(data.size());

    CpioNewcHeader hdr{};
    std::memcpy(hdr.c_magic, magic, 6U);
    write_hex8(hdr.c_ino, ino);
    write_hex8(hdr.c_mode, mode);
    write_hex8(hdr.c_uid, 0U);
    write_hex8(hdr.c_gid, 0U);
    write_hex8(hdr.c_nlink, 1U);
    write_hex8(hdr.c_mtime, 0U);
    write_hex8(hdr.c_filesize, filesize);
    write_hex8(hdr.c_maj, 0U);
    write_hex8(hdr.c_min, 0U);
    write_hex8(hdr.c_rmaj, 0U);
    write_hex8(hdr.c_rmin, 0U);
    write_hex8(hdr.c_namesize, namesize);
    write_hex8(hdr.c_check, 0U);

    const auto* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
    archive.insert(archive.end(), hdr_bytes, hdr_bytes + sizeof(CpioNewcHeader));

    archive.insert(archive.end(), name.begin(), name.end());
    archive.push_back(0U);

    while ((archive.size() % 4U) != 0U) {
        archive.push_back(0U);
    }

    archive.insert(archive.end(), data.begin(), data.end());

    while ((archive.size() % 4U) != 0U) {
        archive.push_back(0U);
    }
}

std::vector<uint8_t> build_sample_cpio(const char* magic = CPIO_NEWC_MAGIC_NOCRC) {
    std::vector<uint8_t> archive;

    const std::string init_content = "milo-initrd-v1\n";
    append_cpio_record(archive, magic, 1U, CPIO_MODE_REGULAR | 0644U, "init.txt",
                       std::vector<uint8_t>(init_content.begin(), init_content.end()));
    append_cpio_record(archive, magic, 2U, CPIO_MODE_DIR | 0755U, "bin", {});

    const std::vector<uint8_t> hello_bytes = {0x7FU, 'E', 'L', 'F', 0x02U, 0x01U, 0x01U};
    append_cpio_record(archive, magic, 3U, CPIO_MODE_REGULAR | 0755U, "./bin/hello", hello_bytes);
    append_cpio_record(archive, magic, 4U, CPIO_MODE_REGULAR | 0600U, "empty.dat", {});
    append_cpio_record(archive, magic, 0U, 0U, CPIO_TRAILER_NAME, {});
    return archive;
}

} // namespace

TEST_CASE("CPIO", "Validation & Lookups for Existing and Missing Paths") {
    EXPECT_EQ(sizeof(CpioNewcHeader), 110U);

    const auto archive_nocrc = build_sample_cpio(CPIO_NEWC_MAGIC_NOCRC);
    const CpioArchive cpio(archive_nocrc.data(), archive_nocrc.size());
    EXPECT_TRUE(cpio.is_valid());

    const auto archive_crc = build_sample_cpio(CPIO_NEWC_MAGIC_CRC);
    const CpioArchive cpio_crc(archive_crc.data(), archive_crc.size());
    EXPECT_TRUE(cpio_crc.is_valid());

    CpioEntry entry{};
    EXPECT_EQ(cpio.lookup("init.txt", entry), CpioStatus::Success);
    EXPECT_EQ(entry.size, 15U);
    EXPECT_TRUE(entry.is_regular_file);
    EXPECT_FALSE(entry.is_directory);
    EXPECT_EQ(std::memcmp(entry.data, "milo-initrd-v1\n", 15U), 0);

    EXPECT_EQ(cpio.lookup("bin", entry), CpioStatus::Success);
    EXPECT_TRUE(entry.is_directory);
    EXPECT_FALSE(entry.is_regular_file);
    EXPECT_EQ(entry.size, 0U);

    EXPECT_EQ(cpio.lookup("empty.dat", entry), CpioStatus::Success);
    EXPECT_TRUE(entry.is_regular_file);
    EXPECT_EQ(entry.size, 0U);

    EXPECT_EQ(cpio.lookup("nonexistent.txt", entry), CpioStatus::FileNotFound);
    EXPECT_EQ(cpio.lookup("", entry), CpioStatus::FileNotFound);
    EXPECT_EQ(cpio.lookup(nullptr, entry), CpioStatus::FileNotFound);

    size_t count = 0U;
    const CpioStatus iter_status = cpio.for_each([&](const CpioEntry&) { count++; });
    EXPECT_EQ(iter_status, CpioStatus::Success);
    EXPECT_EQ(count, 4U);
}

TEST_CASE("CPIO", "Path Normalization & Matching Variations") {
    EXPECT_TRUE(cpio_path_matches("init.txt", "init.txt"));
    EXPECT_TRUE(cpio_path_matches("init.txt", "/init.txt"));
    EXPECT_TRUE(cpio_path_matches("init.txt", "///init.txt"));
    EXPECT_TRUE(cpio_path_matches("init.txt", "./init.txt"));
    EXPECT_TRUE(cpio_path_matches("init.txt", "././init.txt"));
    EXPECT_TRUE(cpio_path_matches("init.txt", "/./init.txt"));
    EXPECT_TRUE(cpio_path_matches("./bin/hello", "bin/hello"));
    EXPECT_TRUE(cpio_path_matches("./bin/hello", "/bin/hello"));
    EXPECT_TRUE(cpio_path_matches("./bin/hello", "///bin/hello"));
    EXPECT_TRUE(cpio_path_matches("./bin/hello", ".///bin/hello"));
    EXPECT_TRUE(cpio_path_matches("/bin/hello", "./bin/hello"));

    EXPECT_FALSE(cpio_path_matches("init.txt", "init.tx"));
    EXPECT_FALSE(cpio_path_matches("init.txt", "init.txt2"));
    EXPECT_FALSE(cpio_path_matches("bin/hello", "bin/hell"));
    EXPECT_FALSE(cpio_path_matches("bin/hello", ""));

    const auto archive = build_sample_cpio();
    CpioEntry entry{};
    EXPECT_EQ(cpio_lookup(archive.data(), archive.size(), "/init.txt", entry), CpioStatus::Success);
    EXPECT_EQ(entry.size, 15U);

    EXPECT_EQ(cpio_lookup(archive.data(), archive.size(), "./init.txt", entry),
              CpioStatus::Success);
    EXPECT_EQ(entry.size, 15U);

    EXPECT_EQ(cpio_lookup(archive.data(), archive.size(), "///bin/hello", entry),
              CpioStatus::Success);
    EXPECT_EQ(entry.size, 7U);

    EXPECT_EQ(cpio_lookup(archive.data(), archive.size(), "././bin/hello", entry),
              CpioStatus::Success);
    EXPECT_EQ(entry.size, 7U);
}

TEST_CASE("CPIO", "Corrupted Header, Alignment Padding & Bounds Rejection") {
    CpioEntry entry{};

    EXPECT_EQ(cpio_lookup(nullptr, 0U, "init.txt", entry), CpioStatus::ArchiveTooSmall);
    EXPECT_EQ(cpio_lookup(nullptr, 256U, "init.txt", entry), CpioStatus::ArchiveTooSmall);

    const auto valid = build_sample_cpio();
    EXPECT_EQ(cpio_lookup(valid.data(), sizeof(CpioNewcHeader) - 1U, "init.txt", entry),
              CpioStatus::ArchiveTooSmall);

    {
        auto bad = valid;
        bad[5] = '9';
        EXPECT_FALSE(CpioArchive(bad.data(), bad.size()).is_valid());
        EXPECT_EQ(cpio_lookup(bad.data(), bad.size(), "init.txt", entry), CpioStatus::InvalidMagic);
    }
    {
        auto bad = valid;
        auto* hdr = reinterpret_cast<CpioNewcHeader*>(bad.data());
        std::memcpy(hdr->c_filesize, "000000ZZ", 8U);
        EXPECT_EQ(cpio_lookup(bad.data(), bad.size(), "init.txt", entry),
                  CpioStatus::MalformedHeader);
    }
    {
        auto bad = valid;
        auto* hdr = reinterpret_cast<CpioNewcHeader*>(bad.data());
        write_hex8(hdr->c_namesize, 0U);
        EXPECT_EQ(cpio_lookup(bad.data(), bad.size(), "init.txt", entry),
                  CpioStatus::MalformedHeader);
    }
    {
        auto bad = valid;
        bad[sizeof(CpioNewcHeader) + 8U] = 'X';
        EXPECT_EQ(cpio_lookup(bad.data(), bad.size(), "init.txt", entry),
                  CpioStatus::MalformedHeader);
    }
    {
        auto bad = valid;
        auto* hdr = reinterpret_cast<CpioNewcHeader*>(bad.data());
        write_hex8(hdr->c_namesize, 4096U);
        EXPECT_EQ(cpio_lookup(bad.data(), bad.size(), "init.txt", entry), CpioStatus::PathTooLong);
    }
    {
        auto bad = valid;
        auto* hdr = reinterpret_cast<CpioNewcHeader*>(bad.data());
        write_hex8(hdr->c_filesize, 0x00100000U);
        EXPECT_EQ(cpio_lookup(bad.data(), bad.size(), "missing", entry),
                  CpioStatus::UnexpectedEndOfArchive);
    }
    {
        EXPECT_EQ(cpio_lookup(valid.data(), valid.size() - 16U, "missing", entry),
                  CpioStatus::UnexpectedEndOfArchive);
    }
    {
        auto bad = valid;
        bad[sizeof(CpioNewcHeader) + 9U] = 0xFFU;
        EXPECT_EQ(cpio_lookup(bad.data(), bad.size(), "init.txt", entry),
                  CpioStatus::CorruptedPadding);
    }
    {
        auto bad = valid;
        bad[120U + 15U] = 0xAAU;
        EXPECT_EQ(cpio_lookup(bad.data(), bad.size(), "missing", entry),
                  CpioStatus::CorruptedPadding);
    }
}