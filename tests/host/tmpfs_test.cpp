#include "fs/rootfs/rootfs.hpp"
#include "fs/tmpfs/tmpfs.hpp"
#include "fs/vfs.hpp"
#include "memory_config.hpp"
#include "mm/pfa.hpp"
#include "mocks/host_hal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <snitch/snitch.hpp>
#include <string_view>

namespace {

using namespace Kernel::FS;

void init_tmpfs_test_env() noexcept {
    HostTest::reset_host_hal();
    HAL::get_mmu().activate();

    Kernel::Boot::MemoryMap mem_map{};
    mem_map.regions[0] = Kernel::Boot::MemoryRegion{
        .base_paddr = HostTest::SIM_RAM_BASE,
        .size = HostTest::SIM_RAM_SIZE,
    };
    mem_map.count = 1U;
    mem_map.min_paddr = HostTest::SIM_RAM_BASE;
    mem_map.max_paddr = HostTest::SIM_RAM_BASE + HostTest::SIM_RAM_SIZE;
    mem_map.total_usable_bytes = HostTest::SIM_RAM_SIZE;

    Kernel::MM::pfa_init(mem_map, HostTest::SIM_RAM_BASE,
                         HostTest::SIM_RAM_BASE + Arch::AArch64::PAGE_SIZE, 0U, 0U, 0U, 0U,
                         nullptr);

    vfs_reset();
    rootfs_reset();
    tmpfs_reset();

    REQUIRE(rootfs_mount_cpio(nullptr, 0U) == kOk);
    REQUIRE(tmpfs_init_and_mount() == kOk);
}

} // namespace

TEST_CASE("tmpfs allocates and reclaims PFA pages on write, truncate, and unlink", "[tmpfs][mm]") {
    init_tmpfs_test_env();
    const size_t baseline_free = Kernel::MM::pfa_free_pages_count();
    REQUIRE(baseline_free > 64U);

    SECTION("consumes exact PFA pages on write and reclaims on O_TRUNC and unlink") {
        File* file = nullptr;
        REQUIRE(vfs_open("/tmp/data.bin", kOpenCreate | kOpenReadWrite, 0644U, &file) == kOk);
        REQUIRE(file != nullptr);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free);

        // Write 100 bytes -> allocates 1 physical page.
        std::array<uint8_t, 100> small_payload{};
        small_payload.fill(0x5AU);
        REQUIRE(vfs_write(file, small_payload.data(), small_payload.size()) == 100);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free - 1U);

        // Extend past 4096-byte boundary -> allocates a 2nd physical page.
        std::array<uint8_t, 5000> large_payload{};
        large_payload.fill(0xA5U);
        REQUIRE(vfs_write(file, large_payload.data(), large_payload.size()) == 5000);
        REQUIRE(file->inode->size == 5100U);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free - 2U);
        REQUIRE(vfs_close(file) == kOk);

        // Re-opening with O_TRUNC frees all allocated PFA frames back to baseline.
        REQUIRE(vfs_open("/tmp/data.bin", kOpenWriteOnly | kOpenTrunc, 0644U, &file) == kOk);
        CHECK(file->inode->size == 0U);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free);

        // Write 3 pages (12288 bytes) and verify unlink + close reclaims all 3 frames.
        std::array<uint8_t, 4096> page_buf{};
        page_buf.fill(0x33U);
        for (size_t idx = 0U; idx < 3U; ++idx) {
            REQUIRE(vfs_write(file, page_buf.data(), page_buf.size()) == 4096);
        }
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free - 3U);
        REQUIRE(vfs_close(file) == kOk);

        REQUIRE(vfs_unlink("/tmp/data.bin") == kOk);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free);
    }

    SECTION("supports sparse offsets and O_APPEND writes") {
        File* file = nullptr;
        REQUIRE(vfs_open("/tmp/sparse.bin", kOpenCreate | kOpenReadWrite, 0644U, &file) == kOk);

        // Seek into page index 2 (offset 8192) and write 4 bytes; only 1 frame is allocated.
        REQUIRE(vfs_lseek(file, 8192, SeekWhence::kSet) == 8192);
        REQUIRE(vfs_write(file, "tail", 4U) == 4);
        CHECK(file->inode->size == 8196U);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free - 1U);

        // Reading from sparse hole at offset 0 returns zeros without allocating frames.
        char hole_buf[8]{};
        std::memset(hole_buf, 0xFF, sizeof(hole_buf));
        REQUIRE(vfs_lseek(file, 0, SeekWhence::kSet) == 0);
        REQUIRE(vfs_read(file, hole_buf, sizeof(hole_buf)) == 8);
        for (char byte_val : hole_buf) {
            CHECK(byte_val == 0);
        }
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free - 1U);
        REQUIRE(vfs_close(file) == kOk);

        // Open with O_APPEND and append 4 more bytes at end (offset 8196).
        REQUIRE(vfs_open("/tmp/sparse.bin", kOpenReadWrite | kOpenAppend, 0644U, &file) == kOk);
        REQUIRE(vfs_write(file, "end!", 4U) == 4);
        CHECK(file->inode->size == 8200U);

        char tail_buf[8]{};
        REQUIRE(vfs_lseek(file, 8192, SeekWhence::kSet) == 8192);
        REQUIRE(vfs_read(file, tail_buf, sizeof(tail_buf)) == 8);
        CHECK(std::string_view(tail_buf, 8) == "tailend!");
        REQUIRE(vfs_close(file) == kOk);

        REQUIRE(vfs_unlink("/tmp/sparse.bin") == kOk);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free);
    }
}

TEST_CASE("tmpfs edge cases: O_EXCL, non-empty rmdir, rename, and unlinked open files",
          "[tmpfs][vfs]") {
    init_tmpfs_test_env();
    const size_t baseline_free = Kernel::MM::pfa_free_pages_count();

    SECTION("enforces O_CREAT | O_EXCL and non-empty directory guards") {
        File* first = nullptr;
        REQUIRE(vfs_open("/tmp/unique.txt", kOpenCreate | kOpenExcl | kOpenReadWrite, 0644U,
                         &first) == kOk);
        REQUIRE(vfs_close(first) == kOk);

        File* duplicate = nullptr;
        REQUIRE(vfs_open("/tmp/unique.txt", kOpenCreate | kOpenExcl | kOpenReadWrite, 0644U,
                         &duplicate) == kEexist);
        CHECK(duplicate == nullptr);

        REQUIRE(vfs_mkdir("/tmp/dir_a", 0755U) == kOk);
        REQUIRE(vfs_mkdir("/tmp/dir_b", 0755U) == kOk);

        File* nested = nullptr;
        REQUIRE(vfs_open("/tmp/dir_a/item.txt", kOpenCreate | kOpenWriteOnly, 0644U, &nested) ==
                kOk);
        REQUIRE(vfs_write(nested, "hello-tmpfs", 11U) == 11);
        REQUIRE(vfs_close(nested) == kOk);

        // Non-empty directory removal fails with -ENOTEMPTY.
        CHECK(vfs_rmdir("/tmp/dir_a") == kEnotempty);

        // Cross-directory rename moves the file and allows removing now-empty dir_a.
        REQUIRE(vfs_rename("/tmp/dir_a/item.txt", "/tmp/dir_b/moved.txt") == kOk);
        CHECK(vfs_rmdir("/tmp/dir_a") == kOk);

        // Readdir on /tmp/dir_b lists moved.txt.
        File* dir_file = nullptr;
        REQUIRE(vfs_open("/tmp/dir_b", kOpenReadOnly | kOpenDirectory, 0U, &dir_file) == kOk);
        DirEntryInfo entry{};
        REQUIRE(vfs_readdir(dir_file, &entry) == kOk);
        CHECK(std::string_view(entry.name) == "moved.txt");
        CHECK(vfs_readdir(dir_file, &entry) == kEnoent);
        REQUIRE(vfs_close(dir_file) == kOk);

        REQUIRE(vfs_unlink("/tmp/dir_b/moved.txt") == kOk);
        REQUIRE(vfs_rmdir("/tmp/dir_b") == kOk);
        REQUIRE(vfs_unlink("/tmp/unique.txt") == kOk);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free);
    }

    SECTION("preserves open File I/O after Dentry unlink and frees pages on close") {
        File* open_file = nullptr;
        REQUIRE(vfs_open("/tmp/ephemeral.txt", kOpenCreate | kOpenReadWrite, 0644U, &open_file) ==
                kOk);
        REQUIRE(vfs_write(open_file, "initial-", 8U) == 8);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free - 1U);

        // Unlink directory entry while descriptor remains open.
        REQUIRE(vfs_unlink("/tmp/ephemeral.txt") == kOk);
        Dentry* lookup_res = nullptr;
        CHECK(vfs_namei("/tmp/ephemeral.txt", nullptr, &lookup_res) == kEnoent);

        // Physical page must remain allocated while open_file is still open.
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free - 1U);

        // Further writes and reads on the unlinked open File succeed.
        REQUIRE(vfs_write(open_file, "ephemeral", 9U) == 9);
        REQUIRE(vfs_lseek(open_file, 0, SeekWhence::kSet) == 0);

        char read_buf[32]{};
        REQUIRE(vfs_read(open_file, read_buf, sizeof(read_buf)) == 17);
        CHECK(std::string_view(read_buf, 17) == "initial-ephemeral");

        // Closing the final open descriptor releases the Inode and its PFA frame.
        REQUIRE(vfs_close(open_file) == kOk);
        CHECK(Kernel::MM::pfa_free_pages_count() == baseline_free);
    }
}