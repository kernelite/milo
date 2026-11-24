#include "drivers/block/block_device.hpp"
#include "fs/ext2/ext2.hpp"
#include "fs/rootfs/rootfs.hpp"
#include "fs/vfs.hpp"

#include <cstdint>
#include <cstring>
#include <snitch/snitch.hpp>
#include <vector>

namespace {

class RamBlockDevice final : public Kernel::Drivers::Block::BlockDevice {
  public:
    explicit RamBlockDevice(size_t total_bytes) : m_storage(total_bytes, 0U) {}

    [[nodiscard]] size_t sector_size() const noexcept override { return 512U; }
    [[nodiscard]] uint64_t sector_count() const noexcept override {
        return m_storage.size() / 512U;
    }

    [[nodiscard]] int read_sectors(uint64_t lba, size_t count, void* dst) noexcept override {
        const size_t off = static_cast<size_t>(lba) * 512U;
        const size_t len = count * 512U;
        if (dst == nullptr || off + len > m_storage.size()) {
            return -22;
        }
        std::memcpy(dst, m_storage.data() + off, len);
        return 0;
    }

    [[nodiscard]] int write_sectors(uint64_t lba, size_t count, const void* src) noexcept override {
        const size_t off = static_cast<size_t>(lba) * 512U;
        const size_t len = count * 512U;
        if (src == nullptr || off + len > m_storage.size()) {
            return -22;
        }
        std::memcpy(m_storage.data() + off, src, len);
        return 0;
    }

    [[nodiscard]] uint8_t* raw_data() noexcept { return m_storage.data(); }

  private:
    std::vector<uint8_t> m_storage;
};

// Programmatically formats a 1 MiB (1024 x 1 KiB blocks) single-group ext2 image
void format_ext2_1k(RamBlockDevice& dev) {
    constexpr uint32_t kBlockSize = 1024U;
    constexpr size_t kBlockSizeSz = kBlockSize;
    constexpr uint32_t kTotalBlocks = 1024U;
    constexpr uint32_t kTotalInodes = 128U;
    constexpr uint32_t kBlockBitmapBlk = 3U;
    constexpr uint32_t kInodeBitmapBlk = 4U;
    constexpr uint32_t kInodeTableBlk = 5U;
    constexpr uint32_t kInodeTableBlocks = (kTotalInodes * 128U) / kBlockSize; // 16 blocks (5..20)
    constexpr uint32_t kRootDirBlk = kInodeTableBlk + kInodeTableBlocks;       // block 21
    constexpr uint32_t kUsedBlocks = kRootDirBlk; // blocks 1..21 used (first_data_block = 1)

    uint8_t* const img = dev.raw_data();
    std::memset(img, 0, static_cast<size_t>(kTotalBlocks) * kBlockSizeSz);

    // 1. Superblock at byte offset 1024 (block 1)
    kernel::fs::Ext2Superblock ext2_sb{};
    ext2_sb.s_inodes_count = kTotalInodes;
    ext2_sb.s_blocks_count = kTotalBlocks;
    ext2_sb.s_free_blocks_count = kTotalBlocks - 1U - kUsedBlocks;
    ext2_sb.s_free_inodes_count = kTotalInodes - 10U; // inodes 1..10 reserved
    ext2_sb.s_first_data_block = 1U;
    ext2_sb.s_log_block_size = 0U; // 1024 bytes
    ext2_sb.s_log_frag_size = 0U;
    ext2_sb.s_blocks_per_group = kTotalBlocks;
    ext2_sb.s_frags_per_group = kTotalBlocks;
    ext2_sb.s_inodes_per_group = kTotalInodes;
    ext2_sb.s_magic = kernel::fs::kExt2SuperMagic;
    ext2_sb.s_state = 1U;
    ext2_sb.s_rev_level = 1U;
    ext2_sb.s_first_ino = 11U;
    ext2_sb.s_inode_size = 128U;
    std::memcpy(img + 1024U, &ext2_sb, sizeof(ext2_sb));

    // 2. BGDT at block 2 (offset 2048)
    kernel::fs::Ext2BlockGroupDesc bgd{};
    bgd.bg_block_bitmap = kBlockBitmapBlk;
    bgd.bg_inode_bitmap = kInodeBitmapBlk;
    bgd.bg_inode_table = kInodeTableBlk;
    bgd.bg_free_blocks_count = static_cast<uint16_t>(ext2_sb.s_free_blocks_count);
    bgd.bg_free_inodes_count = static_cast<uint16_t>(ext2_sb.s_free_inodes_count);
    bgd.bg_used_dirs_count = 1U;
    std::memcpy(img + (size_t{2U} * kBlockSizeSz), &bgd, sizeof(bgd));

    // 3. Block bitmap at block 3: mark blocks 1..21 as allocated (bits 0..20)
    uint8_t* const bmap = img + (static_cast<size_t>(kBlockBitmapBlk) * kBlockSizeSz);
    for (uint32_t bit = 0U; bit < kUsedBlocks; ++bit) {
        bmap[bit / 8U] |= static_cast<uint8_t>(1U << (bit % 8U));
    }

    // 4. Inode bitmap at block 4: mark reserved inodes 1..10 as allocated (bits 0..9)
    uint8_t* const imap = img + (static_cast<size_t>(kInodeBitmapBlk) * kBlockSizeSz);
    for (uint32_t bit = 0U; bit < 10U; ++bit) {
        imap[bit / 8U] |= static_cast<uint8_t>(1U << (bit % 8U));
    }

    // 5. Root inode (#2 -> index 1 in inode table)
    kernel::fs::Ext2DiskInode root_ino{};
    root_ino.i_mode = static_cast<uint16_t>(kernel::fs::kModeDir | 0755U);
    root_ino.i_size = kBlockSize;
    root_ino.i_links_count = 2U;
    root_ino.i_blocks = kBlockSize / 512U;
    root_ino.i_block[0] = kRootDirBlk;
    std::memcpy(img + (static_cast<size_t>(kInodeTableBlk) * kBlockSizeSz) +
                    sizeof(kernel::fs::Ext2DiskInode),
                &root_ino, sizeof(root_ino));

    // 6. Root directory block ('.' and '..')
    uint8_t* const rdir = img + (static_cast<size_t>(kRootDirBlk) * kBlockSizeSz);
    kernel::fs::Ext2DirEntry2 dot{
        .inode = kernel::fs::kExt2RootIno,
        .rec_len = 12U,
        .name_len = 1U,
        .file_type = kernel::fs::kExt2FtDir,
    };
    std::memcpy(rdir, &dot, sizeof(dot));
    rdir[sizeof(dot)] = '.';

    kernel::fs::Ext2DirEntry2 dotdot{
        .inode = kernel::fs::kExt2RootIno,
        .rec_len = static_cast<uint16_t>(kBlockSize - 12U),
        .name_len = 2U,
        .file_type = kernel::fs::kExt2FtDir,
    };
    std::memcpy(rdir + 12U, &dotdot, sizeof(dotdot));
    rdir[12U + sizeof(dotdot)] = '.';
    rdir[12U + sizeof(dotdot) + 1U] = '.';
}

void setup_vfs_with_rootfs() {
    kernel::fs::vfs_reset();
    kernel::fs::rootfs_reset();
    kernel::fs::ext2_reset();
    REQUIRE(kernel::fs::rootfs_mount_cpio(nullptr, 0U) == kernel::fs::kOk);
}

constexpr size_t kTestDiskBytes = size_t{1024U} * size_t{1024U};

} // namespace

TEST_CASE("ext2 rejects invalid superblock magic", "[ext2]") {
    setup_vfs_with_rootfs();
    RamBlockDevice dev(kTestDiskBytes);

    kernel::fs::Superblock* superblock = nullptr;
    REQUIRE(kernel::fs::ext2_mount(&dev, "/mnt/disk", &superblock) == kernel::fs::kEinval);
    REQUIRE(superblock == nullptr);
}

TEST_CASE("ext2 multi-block write across direct-to-singly-indirect and remount persistence",
          "[ext2]") {
    setup_vfs_with_rootfs();
    RamBlockDevice dev(kTestDiskBytes);
    format_ext2_1k(dev);

    kernel::fs::Superblock* superblock = nullptr;
    REQUIRE(kernel::fs::ext2_mount(&dev, "/mnt/disk", &superblock) == kernel::fs::kOk);

    // Write 16 KiB (16 x 1 KiB blocks -> crosses 12-block direct boundary into singly indirect)
    constexpr size_t kPayloadSize = size_t{16U} * size_t{1024U};
    std::vector<uint8_t> tx_buf(kPayloadSize);
    for (size_t i = 0U; i < kPayloadSize; ++i) {
        tx_buf[i] = static_cast<uint8_t>((i * 13U) ^ 0x5AU);
    }

    kernel::fs::File* file = nullptr;
    REQUIRE(kernel::fs::vfs_open("/mnt/disk/big.bin",
                                 kernel::fs::kOpenReadWrite | kernel::fs::kOpenCreate, 0644U,
                                 &file) == kernel::fs::kOk);
    REQUIRE(kernel::fs::vfs_write(file, tx_buf.data(), tx_buf.size()) ==
            static_cast<int64_t>(tx_buf.size()));
    REQUIRE(kernel::fs::vfs_close(file) == kernel::fs::kOk);

    // Unmount and remount over the exact same RamBlockDevice buffer
    REQUIRE(kernel::fs::ext2_umount("/mnt/disk", superblock) == kernel::fs::kOk);
    superblock = nullptr;
    REQUIRE(kernel::fs::ext2_mount(&dev, "/mnt/disk", &superblock) == kernel::fs::kOk);

    std::vector<uint8_t> rx_buf(kPayloadSize, 0U);
    REQUIRE(kernel::fs::vfs_open("/mnt/disk/big.bin", kernel::fs::kOpenReadOnly, 0U, &file) ==
            kernel::fs::kOk);
    REQUIRE(kernel::fs::vfs_read(file, rx_buf.data(), rx_buf.size()) ==
            static_cast<int64_t>(rx_buf.size()));
    REQUIRE(kernel::fs::vfs_close(file) == kernel::fs::kOk);
    REQUIRE(std::memcmp(tx_buf.data(), rx_buf.data(), kPayloadSize) == 0);

    REQUIRE(kernel::fs::ext2_umount("/mnt/disk", superblock) == kernel::fs::kOk);
}

TEST_CASE("ext2 mkdir, dirent insertion, rec_len coalescing on unlink, rename, and rmdir",
          "[ext2]") {
    setup_vfs_with_rootfs();
    RamBlockDevice dev(kTestDiskBytes);
    format_ext2_1k(dev);

    kernel::fs::Superblock* superblock = nullptr;
    REQUIRE(kernel::fs::ext2_mount(&dev, "/mnt/disk", &superblock) == kernel::fs::kOk);

    REQUIRE(kernel::fs::vfs_mkdir("/mnt/disk/sub", 0755U) == kernel::fs::kOk);

    kernel::fs::File* file_a = nullptr;
    kernel::fs::File* file_b = nullptr;
    REQUIRE(kernel::fs::vfs_open("/mnt/disk/sub/a.txt",
                                 kernel::fs::kOpenReadWrite | kernel::fs::kOpenCreate, 0644U,
                                 &file_a) == kernel::fs::kOk);
    REQUIRE(kernel::fs::vfs_write(file_a, "hello", 5U) == 5);
    REQUIRE(kernel::fs::vfs_close(file_a) == kernel::fs::kOk);

    REQUIRE(kernel::fs::vfs_open("/mnt/disk/sub/b.txt",
                                 kernel::fs::kOpenReadWrite | kernel::fs::kOpenCreate, 0644U,
                                 &file_b) == kernel::fs::kOk);
    REQUIRE(kernel::fs::vfs_write(file_b, "world", 5U) == 5);
    REQUIRE(kernel::fs::vfs_close(file_b) == kernel::fs::kOk);

    // Rename b.txt -> c.txt and unlink a.txt (coalesces rec_len with "..")
    REQUIRE(kernel::fs::vfs_rename("/mnt/disk/sub/b.txt", "/mnt/disk/sub/c.txt") ==
            kernel::fs::kOk);
    REQUIRE(kernel::fs::vfs_unlink("/mnt/disk/sub/a.txt") == kernel::fs::kOk);

    // Non-empty rmdir must fail with -ENOTEMPTY
    REQUIRE(kernel::fs::vfs_rmdir("/mnt/disk/sub") == kernel::fs::kEnotempty);

    // Remove remaining file and rmdir
    REQUIRE(kernel::fs::vfs_unlink("/mnt/disk/sub/c.txt") == kernel::fs::kOk);
    REQUIRE(kernel::fs::vfs_rmdir("/mnt/disk/sub") == kernel::fs::kOk);

    // Unmount and remount to verify directory removal persisted on disk
    REQUIRE(kernel::fs::ext2_umount("/mnt/disk", superblock) == kernel::fs::kOk);
    superblock = nullptr;
    REQUIRE(kernel::fs::ext2_mount(&dev, "/mnt/disk", &superblock) == kernel::fs::kOk);

    kernel::fs::Dentry* dentry = nullptr;
    REQUIRE(kernel::fs::vfs_namei("/mnt/disk/sub", nullptr, &dentry) == kernel::fs::kEnoent);

    REQUIRE(kernel::fs::ext2_umount("/mnt/disk", superblock) == kernel::fs::kOk);
}