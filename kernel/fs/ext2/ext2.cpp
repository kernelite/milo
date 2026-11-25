#include "fs/ext2/ext2.hpp"

#include "mm/heap.hpp"

#include <cstring>

namespace kernel::fs {

namespace {

constexpr size_t kMaxExt2Mounts = 4U;
constexpr size_t kMaxBlockGroups = 128U;
constexpr uint32_t kDirectBlocks = 12U;
constexpr uint32_t kSinglyIndirectIdx = 12U;
constexpr uint32_t kDoublyIndirectIdx = 13U;

struct Ext2Fs {
    Kernel::Drivers::Block::BlockDevice* dev{nullptr};
    Ext2Superblock sb{};
    Ext2BlockGroupDesc bgdt[kMaxBlockGroups]{};
    uint32_t block_size{1024U};
    uint32_t inode_size{128U};
    uint32_t group_count{0U};
    uint32_t ptrs_per_block{256U};
    uint32_t bgdt_start_block{2U};
    bool sb_dirty{false};
    bool bgdt_dirty{false};
    bool active{false};
};

Ext2Fs g_ext2_fs[kMaxExt2Mounts]{};

[[nodiscard]] Ext2Fs* alloc_ext2_fs() noexcept {
    for (auto& slot : g_ext2_fs) {
        if (!slot.active) {
            slot = Ext2Fs{};
            slot.active = true;
            return &slot;
        }
    }
    return nullptr;
}

void free_ext2_fs(Ext2Fs* fs) noexcept {
    if (fs != nullptr) {
        *fs = Ext2Fs{};
    }
}

[[nodiscard]] constexpr uint16_t align4_u16(size_t val) noexcept {
    return static_cast<uint16_t>((val + 3U) & ~size_t{3U});
}

[[nodiscard]] constexpr uint16_t dirent_min_len(size_t name_len) noexcept {
    return align4_u16(sizeof(Ext2DirEntry2) + name_len);
}

[[nodiscard]] constexpr uint8_t mode_to_ext2_ft(uint32_t mode) noexcept {
    switch (mode & kModeTypeMask) {
    case kModeReg:
        return kExt2FtRegFile;
    case kModeDir:
        return kExt2FtDir;
    case kModeChr:
        return kExt2FtChrdev;
    case kModeBlk:
        return kExt2FtBlkdev;
    case kModeFifo:
        return kExt2FtFifo;
    case kModeLnk:
        return kExt2FtSymlink;
    default:
        return kExt2FtUnknown;
    }
}

[[nodiscard]] constexpr uint32_t ext2_ft_to_vfs_type(uint8_t ft) noexcept {
    switch (ft) {
    case kExt2FtRegFile:
        return kModeReg;
    case kExt2FtDir:
        return kModeDir;
    case kExt2FtChrdev:
        return kModeChr;
    case kExt2FtBlkdev:
        return kModeBlk;
    case kExt2FtFifo:
        return kModeFifo;
    case kExt2FtSymlink:
        return kModeLnk;
    default:
        return 0U;
    }
}

// ============================================================================
// Low-Level Block I/O
// ============================================================================

[[nodiscard]] int32_t read_block(const Ext2Fs* fs, uint32_t block_no, void* dst) noexcept {
    if (fs == nullptr || fs->dev == nullptr || dst == nullptr ||
        block_no >= fs->sb.s_blocks_count) {
        return kEio;
    }
    const size_t sec_size = fs->dev->sector_size();
    if (sec_size == 0U || (fs->block_size % sec_size) != 0U) {
        return kEio;
    }
    const size_t sectors_per_block = fs->block_size / sec_size;
    const uint64_t start_lba = static_cast<uint64_t>(block_no) * sectors_per_block;
    return (fs->dev->read_sectors(start_lba, sectors_per_block, dst) == 0) ? kOk : kEio;
}

[[nodiscard]] int32_t write_block(const Ext2Fs* fs, uint32_t block_no, const void* src) noexcept {
    if (fs == nullptr || fs->dev == nullptr || src == nullptr ||
        block_no >= fs->sb.s_blocks_count) {
        return kEio;
    }
    const size_t sec_size = fs->dev->sector_size();
    if (sec_size == 0U || (fs->block_size % sec_size) != 0U) {
        return kEio;
    }
    const size_t sectors_per_block = fs->block_size / sec_size;
    const uint64_t start_lba = static_cast<uint64_t>(block_no) * sectors_per_block;
    return (fs->dev->write_sectors(start_lba, sectors_per_block, src) == 0) ? kOk : kEio;
}

[[nodiscard]] int32_t write_superblock(Ext2Fs* fs) noexcept {
    if (fs == nullptr || fs->dev == nullptr) {
        return kEio;
    }
    const size_t sec_size = fs->dev->sector_size();
    if (sec_size == 0U || (sizeof(Ext2Superblock) % sec_size) != 0U ||
        (kExt2SuperblockOffset % sec_size) != 0U) {
        return kEio;
    }
    const uint64_t lba = kExt2SuperblockOffset / sec_size;
    const size_t count = sizeof(Ext2Superblock) / sec_size;
    if (fs->dev->write_sectors(lba, count, &fs->sb) != 0) {
        return kEio;
    }
    fs->sb_dirty = false;
    return kOk;
}

[[nodiscard]] int32_t write_bgdt(Ext2Fs* fs) noexcept {
    if (fs == nullptr || fs->dev == nullptr) {
        return kEio;
    }
    const size_t total_bytes = static_cast<size_t>(fs->group_count) * sizeof(Ext2BlockGroupDesc);
    const uint32_t bgdt_blocks =
        static_cast<uint32_t>((total_bytes + fs->block_size - 1U) / fs->block_size);

    uint8_t block_buf[kExt2MaxBlockSize]{};
    const auto* raw_bgdt = reinterpret_cast<const uint8_t*>(fs->bgdt);

    for (uint32_t blk_idx = 0U; blk_idx < bgdt_blocks; ++blk_idx) {
        const size_t offset = static_cast<size_t>(blk_idx) * fs->block_size;
        const size_t rem = total_bytes - offset;
        const size_t chunk = (rem < fs->block_size) ? rem : fs->block_size;

        if (chunk < fs->block_size) {
            const int32_t rrc = read_block(fs, fs->bgdt_start_block + blk_idx, block_buf);
            if (rrc != kOk) {
                return rrc;
            }
        }
        std::memcpy(block_buf, raw_bgdt + offset, chunk);
        const int32_t wrc = write_block(fs, fs->bgdt_start_block + blk_idx, block_buf);
        if (wrc != kOk) {
            return wrc;
        }
    }

    fs->bgdt_dirty = false;
    return kOk;
}

[[nodiscard]] int32_t sync_metadata(Ext2Fs* fs) noexcept {
    if (fs == nullptr) {
        return kEinval;
    }
    if (fs->bgdt_dirty) {
        const int32_t rc = write_bgdt(fs);
        if (rc != kOk) {
            return rc;
        }
    }
    if (fs->sb_dirty) {
        const int32_t rc = write_superblock(fs);
        if (rc != kOk) {
            return rc;
        }
    }
    return kOk;
}

// ============================================================================
// Bitmap Allocators (ext2_balloc / ext2_bfree / ext2_ialloc / ext2_ifree)
// ============================================================================

[[nodiscard]] int32_t ext2_balloc(Ext2Fs* fs, uint32_t preferred_group,
                                  uint32_t& out_block) noexcept {
    out_block = 0U;
    if (fs == nullptr || fs->sb.s_free_blocks_count == 0U) {
        return kEnospc;
    }

    uint8_t bitmap[kExt2MaxBlockSize]{};
    const uint32_t start_group = (preferred_group < fs->group_count) ? preferred_group : 0U;

    for (uint32_t offset = 0U; offset < fs->group_count; ++offset) {
        const uint32_t grp = (start_group + offset) % fs->group_count;
        if (fs->bgdt[grp].bg_free_blocks_count == 0U) {
            continue;
        }

        const int32_t rrc = read_block(fs, fs->bgdt[grp].bg_block_bitmap, bitmap);
        if (rrc != kOk) {
            return rrc;
        }

        const uint32_t group_first_block =
            fs->sb.s_first_data_block + (grp * fs->sb.s_blocks_per_group);
        uint32_t blocks_in_group = fs->sb.s_blocks_per_group;
        if (group_first_block + blocks_in_group > fs->sb.s_blocks_count) {
            blocks_in_group = fs->sb.s_blocks_count - group_first_block;
        }

        for (uint32_t bit = 0U; bit < blocks_in_group; ++bit) {
            const uint32_t byte_idx = bit / 8U;
            const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8U));
            if ((bitmap[byte_idx] & mask) == 0U) {
                bitmap[byte_idx] |= mask;
                const int32_t wrc = write_block(fs, fs->bgdt[grp].bg_block_bitmap, bitmap);
                if (wrc != kOk) {
                    return wrc;
                }

                --fs->bgdt[grp].bg_free_blocks_count;
                --fs->sb.s_free_blocks_count;
                fs->bgdt_dirty = true;
                fs->sb_dirty = true;

                const uint32_t allocated = group_first_block + bit;

                // Zero-initialize the newly allocated block on disk
                uint8_t zero_buf[kExt2MaxBlockSize]{};
                const int32_t zrc = write_block(fs, allocated, zero_buf);
                if (zrc != kOk) {
                    return zrc;
                }

                out_block = allocated;
                return kOk;
            }
        }
    }

    return kEnospc;
}

[[nodiscard]] int32_t ext2_bfree(Ext2Fs* fs, uint32_t block_no) noexcept {
    if (fs == nullptr || block_no < fs->sb.s_first_data_block ||
        block_no >= fs->sb.s_blocks_count) {
        return kEinval;
    }

    const uint32_t rel = block_no - fs->sb.s_first_data_block;
    const uint32_t grp = rel / fs->sb.s_blocks_per_group;
    const uint32_t bit = rel % fs->sb.s_blocks_per_group;
    if (grp >= fs->group_count) {
        return kEinval;
    }

    uint8_t bitmap[kExt2MaxBlockSize]{};
    const int32_t rrc = read_block(fs, fs->bgdt[grp].bg_block_bitmap, bitmap);
    if (rrc != kOk) {
        return rrc;
    }

    const uint32_t byte_idx = bit / 8U;
    const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8U));
    if ((bitmap[byte_idx] & mask) == 0U) {
        return kOk; // Already free
    }

    bitmap[byte_idx] = static_cast<uint8_t>(bitmap[byte_idx] & ~static_cast<uint32_t>(mask));
    const int32_t wrc = write_block(fs, fs->bgdt[grp].bg_block_bitmap, bitmap);
    if (wrc != kOk) {
        return wrc;
    }

    ++fs->bgdt[grp].bg_free_blocks_count;
    ++fs->sb.s_free_blocks_count;
    fs->bgdt_dirty = true;
    fs->sb_dirty = true;
    return kOk;
}

[[nodiscard]] int32_t ext2_ialloc(Ext2Fs* fs, uint32_t preferred_group, bool is_dir,
                                  uint32_t& out_ino) noexcept {
    out_ino = 0U;
    if (fs == nullptr || fs->sb.s_free_inodes_count == 0U) {
        return kEnospc;
    }

    uint8_t bitmap[kExt2MaxBlockSize]{};
    const uint32_t start_group = (preferred_group < fs->group_count) ? preferred_group : 0U;

    for (uint32_t offset = 0U; offset < fs->group_count; ++offset) {
        const uint32_t grp = (start_group + offset) % fs->group_count;
        if (fs->bgdt[grp].bg_free_inodes_count == 0U) {
            continue;
        }

        const int32_t rrc = read_block(fs, fs->bgdt[grp].bg_inode_bitmap, bitmap);
        if (rrc != kOk) {
            return rrc;
        }

        for (uint32_t bit = 0U; bit < fs->sb.s_inodes_per_group; ++bit) {
            const uint32_t candidate_ino = (grp * fs->sb.s_inodes_per_group) + bit + 1U;
            if (candidate_ino > fs->sb.s_inodes_count) {
                break;
            }
            // Skip reserved inodes (< s_first_ino, typically 1..10)
            const uint32_t first_ino =
                (fs->sb.s_rev_level >= 1U && fs->sb.s_first_ino > 0U) ? fs->sb.s_first_ino : 11U;
            if (candidate_ino < first_ino) {
                continue;
            }

            const uint32_t byte_idx = bit / 8U;
            const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8U));
            if ((bitmap[byte_idx] & mask) == 0U) {
                bitmap[byte_idx] |= mask;
                const int32_t wrc = write_block(fs, fs->bgdt[grp].bg_inode_bitmap, bitmap);
                if (wrc != kOk) {
                    return wrc;
                }

                --fs->bgdt[grp].bg_free_inodes_count;
                if (is_dir) {
                    ++fs->bgdt[grp].bg_used_dirs_count;
                }
                --fs->sb.s_free_inodes_count;
                fs->bgdt_dirty = true;
                fs->sb_dirty = true;

                out_ino = candidate_ino;
                return kOk;
            }
        }
    }

    return kEnospc;
}

[[nodiscard]] int32_t ext2_ifree(Ext2Fs* fs, uint32_t ino, bool is_dir) noexcept {
    if (fs == nullptr || ino == 0U || ino > fs->sb.s_inodes_count) {
        return kEinval;
    }

    const uint32_t idx = ino - 1U;
    const uint32_t grp = idx / fs->sb.s_inodes_per_group;
    const uint32_t bit = idx % fs->sb.s_inodes_per_group;
    if (grp >= fs->group_count) {
        return kEinval;
    }

    uint8_t bitmap[kExt2MaxBlockSize]{};
    const int32_t rrc = read_block(fs, fs->bgdt[grp].bg_inode_bitmap, bitmap);
    if (rrc != kOk) {
        return rrc;
    }

    const uint32_t byte_idx = bit / 8U;
    const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8U));
    if ((bitmap[byte_idx] & mask) == 0U) {
        return kOk;
    }

    bitmap[byte_idx] = static_cast<uint8_t>(bitmap[byte_idx] & ~static_cast<uint32_t>(mask));
    const int32_t wrc = write_block(fs, fs->bgdt[grp].bg_inode_bitmap, bitmap);
    if (wrc != kOk) {
        return wrc;
    }

    ++fs->bgdt[grp].bg_free_inodes_count;
    if (is_dir && fs->bgdt[grp].bg_used_dirs_count > 0U) {
        --fs->bgdt[grp].bg_used_dirs_count;
    }
    ++fs->sb.s_free_inodes_count;
    fs->bgdt_dirty = true;
    fs->sb_dirty = true;
    return kOk;
}

// ============================================================================
// On-Disk Inode Read / Write
// ============================================================================

[[nodiscard]] int32_t read_disk_inode(const Ext2Fs* fs, uint32_t ino,
                                      Ext2DiskInode& out_inode) noexcept {
    if (fs == nullptr || ino == 0U || ino > fs->sb.s_inodes_count) {
        return kEinval;
    }
    const uint32_t idx = ino - 1U;
    const uint32_t grp = idx / fs->sb.s_inodes_per_group;
    const uint32_t local_idx = idx % fs->sb.s_inodes_per_group;
    if (grp >= fs->group_count) {
        return kEinval;
    }

    const uint64_t byte_offset = static_cast<uint64_t>(local_idx) * fs->inode_size;
    const uint32_t block_no =
        fs->bgdt[grp].bg_inode_table + static_cast<uint32_t>(byte_offset / fs->block_size);
    const size_t offset_in_block = static_cast<size_t>(byte_offset % fs->block_size);

    uint8_t block_buf[kExt2MaxBlockSize]{};
    const int32_t rc = read_block(fs, block_no, block_buf);
    if (rc != kOk) {
        return rc;
    }

    std::memcpy(&out_inode, block_buf + offset_in_block, sizeof(Ext2DiskInode));
    return kOk;
}

[[nodiscard]] int32_t write_disk_inode(const Ext2Fs* fs, uint32_t ino,
                                       const Ext2DiskInode& disk_inode) noexcept {
    if (fs == nullptr || ino == 0U || ino > fs->sb.s_inodes_count) {
        return kEinval;
    }
    const uint32_t idx = ino - 1U;
    const uint32_t grp = idx / fs->sb.s_inodes_per_group;
    const uint32_t local_idx = idx % fs->sb.s_inodes_per_group;
    if (grp >= fs->group_count) {
        return kEinval;
    }

    const uint64_t byte_offset = static_cast<uint64_t>(local_idx) * fs->inode_size;
    const uint32_t block_no =
        fs->bgdt[grp].bg_inode_table + static_cast<uint32_t>(byte_offset / fs->block_size);
    const size_t offset_in_block = static_cast<size_t>(byte_offset % fs->block_size);

    uint8_t block_buf[kExt2MaxBlockSize]{};
    const int32_t rrc = read_block(fs, block_no, block_buf);
    if (rrc != kOk) {
        return rrc;
    }

    std::memcpy(block_buf + offset_in_block, &disk_inode, sizeof(Ext2DiskInode));
    return write_block(fs, block_no, block_buf);
}

// ============================================================================
// Block Pointer Resolution (Direct, Singly Indirect, Doubly Indirect)
// ============================================================================

[[nodiscard]] uint32_t inode_sectors_per_block(const Ext2Fs* fs) noexcept {
    return fs->block_size / 512U;
}

[[nodiscard]] int32_t get_or_alloc_block(Ext2Fs* fs, uint32_t ino, Ext2DiskInode& disk_inode,
                                         uint32_t logical_blk, bool allocate,
                                         uint32_t& out_phys_blk, bool& inode_modified) noexcept {
    out_phys_blk = 0U;
    const uint32_t pref_grp = (ino > 0U) ? ((ino - 1U) / fs->sb.s_inodes_per_group) : 0U;
    const uint32_t ppb = fs->ptrs_per_block;

    // 1. Direct blocks (0..11)
    if (logical_blk < kDirectBlocks) {
        if (disk_inode.i_block[logical_blk] == 0U) {
            if (!allocate) {
                return kOk;
            }
            uint32_t new_blk = 0U;
            const int32_t rc = ext2_balloc(fs, pref_grp, new_blk);
            if (rc != kOk) {
                return rc;
            }
            disk_inode.i_block[logical_blk] = new_blk;
            disk_inode.i_blocks += inode_sectors_per_block(fs);
            inode_modified = true;
        }
        out_phys_blk = disk_inode.i_block[logical_blk];
        return kOk;
    }

    // 2. Singly indirect blocks (12 .. 12 + ppb - 1)
    uint32_t rel = logical_blk - kDirectBlocks;
    if (rel < ppb) {
        if (disk_inode.i_block[kSinglyIndirectIdx] == 0U) {
            if (!allocate) {
                return kOk;
            }
            uint32_t ind_blk = 0U;
            const int32_t rc = ext2_balloc(fs, pref_grp, ind_blk);
            if (rc != kOk) {
                return rc;
            }
            disk_inode.i_block[kSinglyIndirectIdx] = ind_blk;
            disk_inode.i_blocks += inode_sectors_per_block(fs);
            inode_modified = true;
        }

        uint32_t ind_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
        const int32_t rrc = read_block(fs, disk_inode.i_block[kSinglyIndirectIdx], ind_table);
        if (rrc != kOk) {
            return rrc;
        }

        if (ind_table[rel] == 0U) {
            if (!allocate) {
                return kOk;
            }
            uint32_t new_blk = 0U;
            const int32_t rc = ext2_balloc(fs, pref_grp, new_blk);
            if (rc != kOk) {
                return rc;
            }
            ind_table[rel] = new_blk;
            const int32_t wrc = write_block(fs, disk_inode.i_block[kSinglyIndirectIdx], ind_table);
            if (wrc != kOk) {
                return wrc;
            }
            disk_inode.i_blocks += inode_sectors_per_block(fs);
            inode_modified = true;
        }

        out_phys_blk = ind_table[rel];
        return kOk;
    }

    // 3. Doubly indirect blocks (12 + ppb .. 12 + ppb + ppb*ppb - 1)
    rel -= ppb;
    const uint64_t dbl_cap = static_cast<uint64_t>(ppb) * ppb;
    if (static_cast<uint64_t>(rel) < dbl_cap) {
        const uint32_t l1_idx = rel / ppb;
        const uint32_t l2_idx = rel % ppb;

        if (disk_inode.i_block[kDoublyIndirectIdx] == 0U) {
            if (!allocate) {
                return kOk;
            }
            uint32_t dbl_blk = 0U;
            const int32_t rc = ext2_balloc(fs, pref_grp, dbl_blk);
            if (rc != kOk) {
                return rc;
            }
            disk_inode.i_block[kDoublyIndirectIdx] = dbl_blk;
            disk_inode.i_blocks += inode_sectors_per_block(fs);
            inode_modified = true;
        }

        uint32_t l1_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
        int32_t rc = read_block(fs, disk_inode.i_block[kDoublyIndirectIdx], l1_table);
        if (rc != kOk) {
            return rc;
        }

        if (l1_table[l1_idx] == 0U) {
            if (!allocate) {
                return kOk;
            }
            uint32_t l2_blk = 0U;
            rc = ext2_balloc(fs, pref_grp, l2_blk);
            if (rc != kOk) {
                return rc;
            }
            l1_table[l1_idx] = l2_blk;
            rc = write_block(fs, disk_inode.i_block[kDoublyIndirectIdx], l1_table);
            if (rc != kOk) {
                return rc;
            }
            disk_inode.i_blocks += inode_sectors_per_block(fs);
            inode_modified = true;
        }

        uint32_t l2_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
        rc = read_block(fs, l1_table[l1_idx], l2_table);
        if (rc != kOk) {
            return rc;
        }

        if (l2_table[l2_idx] == 0U) {
            if (!allocate) {
                return kOk;
            }
            uint32_t new_blk = 0U;
            rc = ext2_balloc(fs, pref_grp, new_blk);
            if (rc != kOk) {
                return rc;
            }
            l2_table[l2_idx] = new_blk;
            rc = write_block(fs, l1_table[l1_idx], l2_table);
            if (rc != kOk) {
                return rc;
            }
            disk_inode.i_blocks += inode_sectors_per_block(fs);
            inode_modified = true;
        }

        out_phys_blk = l2_table[l2_idx];
        return kOk;
    }

    return kEnospc;
}

void free_all_inode_blocks(Ext2Fs* fs, Ext2DiskInode& disk_inode) noexcept {
    // 1. Direct blocks
    for (uint32_t i = 0U; i < kDirectBlocks; ++i) {
        if (disk_inode.i_block[i] != 0U) {
            (void)ext2_bfree(fs, disk_inode.i_block[i]);
            disk_inode.i_block[i] = 0U;
        }
    }

    // 2. Singly indirect
    if (disk_inode.i_block[kSinglyIndirectIdx] != 0U) {
        uint32_t ind_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
        if (read_block(fs, disk_inode.i_block[kSinglyIndirectIdx], ind_table) == kOk) {
            for (uint32_t i = 0U; i < fs->ptrs_per_block; ++i) {
                if (ind_table[i] != 0U) {
                    (void)ext2_bfree(fs, ind_table[i]);
                }
            }
        }
        (void)ext2_bfree(fs, disk_inode.i_block[kSinglyIndirectIdx]);
        disk_inode.i_block[kSinglyIndirectIdx] = 0U;
    }

    // 3. Doubly indirect
    if (disk_inode.i_block[kDoublyIndirectIdx] != 0U) {
        uint32_t l1_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
        if (read_block(fs, disk_inode.i_block[kDoublyIndirectIdx], l1_table) == kOk) {
            uint32_t l2_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
            for (uint32_t i = 0U; i < fs->ptrs_per_block; ++i) {
                if (l1_table[i] != 0U) {
                    if (read_block(fs, l1_table[i], l2_table) == kOk) {
                        for (uint32_t j = 0U; j < fs->ptrs_per_block; ++j) {
                            if (l2_table[j] != 0U) {
                                (void)ext2_bfree(fs, l2_table[j]);
                            }
                        }
                    }
                    (void)ext2_bfree(fs, l1_table[i]);
                }
            }
        }
        (void)ext2_bfree(fs, disk_inode.i_block[kDoublyIndirectIdx]);
        disk_inode.i_block[kDoublyIndirectIdx] = 0U;
    }

    disk_inode.i_blocks = 0U;
}

// ============================================================================
// Directory Entry Helpers
// ============================================================================

[[nodiscard]] int32_t dir_find_entry(Ext2Fs* fs, uint32_t dir_ino, const Ext2DiskInode& dir_disk,
                                     StringView name, uint32_t& out_child_ino) noexcept {
    out_child_ino = 0U;
    const uint32_t total_blocks =
        static_cast<uint32_t>((dir_disk.i_size + fs->block_size - 1U) / fs->block_size);

    uint8_t block_buf[kExt2MaxBlockSize]{};
    Ext2DiskInode mutable_copy = dir_disk;
    bool unused_mod = false;

    for (uint32_t lblk = 0U; lblk < total_blocks; ++lblk) {
        uint32_t pblk = 0U;
        const int32_t brc =
            get_or_alloc_block(fs, dir_ino, mutable_copy, lblk, false, pblk, unused_mod);
        if (brc != kOk) {
            return brc;
        }
        if (pblk == 0U) {
            continue;
        }
        const int32_t rrc = read_block(fs, pblk, block_buf);
        if (rrc != kOk) {
            return rrc;
        }

        size_t off = 0U;
        while (off + sizeof(Ext2DirEntry2) <= fs->block_size) {
            Ext2DirEntry2 ent{};
            std::memcpy(&ent, block_buf + off, sizeof(ent));
            if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > fs->block_size) {
                break;
            }
            if (ent.inode != 0U && ent.name_len == name.size() &&
                off + sizeof(Ext2DirEntry2) + ent.name_len <= fs->block_size) {
                const auto* ename = reinterpret_cast<const char*>(block_buf + off + sizeof(ent));
                if (StringView(ename, ent.name_len) == name) {
                    out_child_ino = ent.inode;
                    return kOk;
                }
            }
            off += ent.rec_len;
        }
    }

    return kEnoent;
}

[[nodiscard]] int32_t dir_is_empty(Ext2Fs* fs, uint32_t dir_ino, const Ext2DiskInode& dir_disk,
                                   bool& out_empty) noexcept {
    out_empty = true;
    const uint32_t total_blocks =
        static_cast<uint32_t>((dir_disk.i_size + fs->block_size - 1U) / fs->block_size);

    uint8_t block_buf[kExt2MaxBlockSize]{};
    Ext2DiskInode mutable_copy = dir_disk;
    bool unused_mod = false;

    for (uint32_t lblk = 0U; lblk < total_blocks; ++lblk) {
        uint32_t pblk = 0U;
        const int32_t brc =
            get_or_alloc_block(fs, dir_ino, mutable_copy, lblk, false, pblk, unused_mod);
        if (brc != kOk) {
            return brc;
        }
        if (pblk == 0U) {
            continue;
        }
        const int32_t rrc = read_block(fs, pblk, block_buf);
        if (rrc != kOk) {
            return rrc;
        }

        size_t off = 0U;
        while (off + sizeof(Ext2DirEntry2) <= fs->block_size) {
            Ext2DirEntry2 ent{};
            std::memcpy(&ent, block_buf + off, sizeof(ent));
            if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > fs->block_size) {
                break;
            }
            if (ent.inode != 0U && off + sizeof(Ext2DirEntry2) + ent.name_len <= fs->block_size) {
                const auto* ename = reinterpret_cast<const char*>(block_buf + off + sizeof(ent));
                const StringView sv(ename, ent.name_len);
                if (sv != "." && sv != "..") {
                    out_empty = false;
                    return kOk;
                }
            }
            off += ent.rec_len;
        }
    }

    return kOk;
}

[[nodiscard]] int32_t dir_add_entry(Ext2Fs* fs, uint32_t dir_ino, Ext2DiskInode& dir_disk,
                                    StringView name, uint32_t child_ino,
                                    uint8_t file_type) noexcept {
    if (name.empty() || name.size() > 255U) {
        return kEnametoolong;
    }

    const uint16_t needed = dirent_min_len(name.size());
    const uint32_t total_blocks =
        static_cast<uint32_t>((dir_disk.i_size + fs->block_size - 1U) / fs->block_size);

    uint8_t block_buf[kExt2MaxBlockSize]{};
    bool inode_mod = false;

    for (uint32_t lblk = 0U; lblk < total_blocks; ++lblk) {
        uint32_t pblk = 0U;
        const int32_t brc = get_or_alloc_block(fs, dir_ino, dir_disk, lblk, false, pblk, inode_mod);
        if (brc != kOk) {
            return brc;
        }
        if (pblk == 0U) {
            continue;
        }
        const int32_t rrc = read_block(fs, pblk, block_buf);
        if (rrc != kOk) {
            return rrc;
        }

        size_t off = 0U;
        while (off + sizeof(Ext2DirEntry2) <= fs->block_size) {
            Ext2DirEntry2 ent{};
            std::memcpy(&ent, block_buf + off, sizeof(ent));
            if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > fs->block_size) {
                break;
            }

            // Case 1: Reuse an unused slot (inode == 0) if it fits
            if (ent.inode == 0U && ent.rec_len >= needed) {
                ent.inode = child_ino;
                ent.name_len = static_cast<uint8_t>(name.size());
                ent.file_type = file_type;
                std::memcpy(block_buf + off, &ent, sizeof(ent));
                std::memcpy(block_buf + off + sizeof(ent), name.data(), name.size());
                return write_block(fs, pblk, block_buf);
            }

            // Case 2: Split trailing slack space in an existing entry
            const uint16_t actual_len = dirent_min_len(ent.name_len);
            if (ent.inode != 0U && ent.rec_len >= actual_len + needed) {
                const uint16_t new_rec_len = static_cast<uint16_t>(ent.rec_len - actual_len);
                ent.rec_len = actual_len;
                std::memcpy(block_buf + off, &ent, sizeof(ent));

                const size_t new_off = off + actual_len;
                Ext2DirEntry2 new_ent{
                    .inode = child_ino,
                    .rec_len = new_rec_len,
                    .name_len = static_cast<uint8_t>(name.size()),
                    .file_type = file_type,
                };
                std::memcpy(block_buf + new_off, &new_ent, sizeof(new_ent));
                std::memcpy(block_buf + new_off + sizeof(new_ent), name.data(), name.size());
                return write_block(fs, pblk, block_buf);
            }

            off += ent.rec_len;
        }
    }

    // Case 3: Allocate a brand new directory block
    uint32_t new_pblk = 0U;
    const int32_t brc =
        get_or_alloc_block(fs, dir_ino, dir_disk, total_blocks, true, new_pblk, inode_mod);
    if (brc != kOk) {
        return brc;
    }

    std::memset(block_buf, 0, fs->block_size);
    Ext2DirEntry2 new_ent{
        .inode = child_ino,
        .rec_len = static_cast<uint16_t>(fs->block_size),
        .name_len = static_cast<uint8_t>(name.size()),
        .file_type = file_type,
    };
    std::memcpy(block_buf, &new_ent, sizeof(new_ent));
    std::memcpy(block_buf + sizeof(new_ent), name.data(), name.size());

    const int32_t wrc = write_block(fs, new_pblk, block_buf);
    if (wrc != kOk) {
        return wrc;
    }

    dir_disk.i_size += fs->block_size;
    return write_disk_inode(fs, dir_ino, dir_disk);
}

[[nodiscard]] int32_t dir_remove_entry(Ext2Fs* fs, uint32_t dir_ino, const Ext2DiskInode& dir_disk,
                                       StringView name) noexcept {
    const uint32_t total_blocks =
        static_cast<uint32_t>((dir_disk.i_size + fs->block_size - 1U) / fs->block_size);

    uint8_t block_buf[kExt2MaxBlockSize]{};
    Ext2DiskInode mutable_copy = dir_disk;
    bool unused_mod = false;

    for (uint32_t lblk = 0U; lblk < total_blocks; ++lblk) {
        uint32_t pblk = 0U;
        const int32_t brc =
            get_or_alloc_block(fs, dir_ino, mutable_copy, lblk, false, pblk, unused_mod);
        if (brc != kOk) {
            return brc;
        }
        if (pblk == 0U) {
            continue;
        }
        const int32_t rrc = read_block(fs, pblk, block_buf);
        if (rrc != kOk) {
            return rrc;
        }

        size_t off = 0U;
        size_t prev_off = kExt2MaxBlockSize;
        while (off + sizeof(Ext2DirEntry2) <= fs->block_size) {
            Ext2DirEntry2 ent{};
            std::memcpy(&ent, block_buf + off, sizeof(ent));
            if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > fs->block_size) {
                break;
            }
            if (ent.inode != 0U && ent.name_len == name.size() &&
                off + sizeof(Ext2DirEntry2) + ent.name_len <= fs->block_size) {
                const auto* ename = reinterpret_cast<const char*>(block_buf + off + sizeof(ent));
                if (StringView(ename, ent.name_len) == name) {
                    if (prev_off != kExt2MaxBlockSize) {
                        // Coalesce rec_len into preceding entry
                        Ext2DirEntry2 prev_ent{};
                        std::memcpy(&prev_ent, block_buf + prev_off, sizeof(prev_ent));
                        prev_ent.rec_len = static_cast<uint16_t>(prev_ent.rec_len + ent.rec_len);
                        std::memcpy(block_buf + prev_off, &prev_ent, sizeof(prev_ent));
                    } else {
                        // First entry in block: mark unused
                        ent.inode = 0U;
                        std::memcpy(block_buf + off, &ent, sizeof(ent));
                    }
                    return write_block(fs, pblk, block_buf);
                }
            }
            prev_off = off;
            off += ent.rec_len;
        }
    }

    return kEnoent;
}

[[nodiscard]] int32_t dir_update_dotdot(Ext2Fs* fs, const Ext2DiskInode& dir_disk,
                                        uint32_t new_parent_ino) noexcept {
    if (dir_disk.i_block[0] == 0U) {
        return kEio;
    }
    uint8_t block_buf[kExt2MaxBlockSize]{};
    const int32_t rrc = read_block(fs, dir_disk.i_block[0], block_buf);
    if (rrc != kOk) {
        return rrc;
    }

    size_t off = 0U;
    while (off + sizeof(Ext2DirEntry2) <= fs->block_size) {
        Ext2DirEntry2 ent{};
        std::memcpy(&ent, block_buf + off, sizeof(ent));
        if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > fs->block_size) {
            break;
        }
        if (ent.inode != 0U && ent.name_len == 2U &&
            off + sizeof(Ext2DirEntry2) + 2U <= fs->block_size) {
            const auto* ename = reinterpret_cast<const char*>(block_buf + off + sizeof(ent));
            if (ename[0] == '.' && ename[1] == '.') {
                ent.inode = new_parent_ino;
                std::memcpy(block_buf + off, &ent, sizeof(ent));
                return write_block(fs, dir_disk.i_block[0], block_buf);
            }
        }
        off += ent.rec_len;
    }
    return kEnoent;
}

// ============================================================================
// VFS Operations
// ============================================================================

extern const InodeOperations kExt2InodeOps;
extern const FileOperations kExt2FileOps;

[[nodiscard]] Inode* load_vfs_inode(Superblock* sb, uint32_t ino) noexcept {
    auto* const fs = static_cast<Ext2Fs*>(sb->fs_private);
    Ext2DiskInode disk{};
    if (read_disk_inode(fs, ino, disk) != kOk) {
        return nullptr;
    }

    Inode* inode = vfs_alloc_inode(sb);
    if (inode == nullptr) {
        return nullptr;
    }
    inode->ino = ino;
    inode->mode = disk.i_mode;
    inode->uid = disk.i_uid;
    inode->gid = disk.i_gid;
    inode->size = disk.i_size;
    inode->nlink = disk.i_links_count;
    inode->i_ops = &kExt2InodeOps;
    inode->f_ops = &kExt2FileOps;
    return inode;
}

int32_t ext2_sync_fs(Superblock* sb) noexcept {
    if (sb == nullptr || sb->fs_private == nullptr) {
        return kEinval;
    }
    return sync_metadata(static_cast<Ext2Fs*>(sb->fs_private));
}

void ext2_destroy_inode(Inode* inode) noexcept {
    if (inode == nullptr) {
        return;
    }
    if (inode->sb != nullptr && inode->sb->fs_private != nullptr && inode->nlink == 0U &&
        inode->ino != 0U) {
        auto* const fs = static_cast<Ext2Fs*>(inode->sb->fs_private);
        Ext2DiskInode disk{};
        if (read_disk_inode(fs, static_cast<uint32_t>(inode->ino), disk) == kOk) {
            free_all_inode_blocks(fs, disk);
            disk.i_size = 0U;
            disk.i_links_count = 0U;
            (void)write_disk_inode(fs, static_cast<uint32_t>(inode->ino), disk);
        }
        (void)ext2_ifree(fs, static_cast<uint32_t>(inode->ino), inode->is_dir());
    }
    *inode = Inode{};
}

int32_t ext2_lookup(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || dir->sb == nullptr ||
        dir->sb->fs_private == nullptr) {
        return kEinval;
    }
    auto* const fs = static_cast<Ext2Fs*>(dir->sb->fs_private);

    Ext2DiskInode dir_disk{};
    const int32_t rrc = read_disk_inode(fs, static_cast<uint32_t>(dir->ino), dir_disk);
    if (rrc != kOk) {
        return rrc;
    }

    uint32_t child_ino = 0U;
    const int32_t frc = dir_find_entry(fs, static_cast<uint32_t>(dir->ino), dir_disk,
                                       target->name_view(), child_ino);
    if (frc != kOk) {
        return frc;
    }

    Inode* child_vfs = load_vfs_inode(dir->sb, child_ino);
    if (child_vfs == nullptr) {
        return kEnomem;
    }
    target->inode = child_vfs;
    return kOk;
}

int32_t ext2_create(Inode* dir, Dentry* target, uint32_t mode) noexcept {
    if (dir == nullptr || target == nullptr || dir->sb == nullptr ||
        dir->sb->fs_private == nullptr) {
        return kEinval;
    }
    auto* const fs = static_cast<Ext2Fs*>(dir->sb->fs_private);
    const uint32_t dir_ino = static_cast<uint32_t>(dir->ino);
    const uint32_t pref_grp = (dir_ino - 1U) / fs->sb.s_inodes_per_group;

    uint32_t new_ino = 0U;
    int32_t rc = ext2_ialloc(fs, pref_grp, false, new_ino);
    if (rc != kOk) {
        return rc;
    }

    Ext2DiskInode new_disk{};
    new_disk.i_mode = static_cast<uint16_t>(kModeReg | (mode & 07777U));
    new_disk.i_links_count = 1U;
    rc = write_disk_inode(fs, new_ino, new_disk);
    if (rc != kOk) {
        (void)ext2_ifree(fs, new_ino, false);
        return rc;
    }

    Ext2DiskInode dir_disk{};
    rc = read_disk_inode(fs, dir_ino, dir_disk);
    if (rc != kOk) {
        (void)ext2_ifree(fs, new_ino, false);
        return rc;
    }

    rc = dir_add_entry(fs, dir_ino, dir_disk, target->name_view(), new_ino, kExt2FtRegFile);
    if (rc != kOk) {
        (void)ext2_ifree(fs, new_ino, false);
        return rc;
    }

    dir->size = dir_disk.i_size;
    Inode* vfs_ino = load_vfs_inode(dir->sb, new_ino);
    if (vfs_ino == nullptr) {
        return kEnomem;
    }
    target->inode = vfs_ino;
    return kOk;
}

int32_t ext2_mkdir(Inode* dir, Dentry* target, uint32_t mode) noexcept {
    if (dir == nullptr || target == nullptr || dir->sb == nullptr ||
        dir->sb->fs_private == nullptr) {
        return kEinval;
    }
    auto* const fs = static_cast<Ext2Fs*>(dir->sb->fs_private);
    const uint32_t parent_ino = static_cast<uint32_t>(dir->ino);
    const uint32_t pref_grp = (parent_ino - 1U) / fs->sb.s_inodes_per_group;

    uint32_t new_ino = 0U;
    int32_t rc = ext2_ialloc(fs, pref_grp, true, new_ino);
    if (rc != kOk) {
        return rc;
    }

    uint32_t dir_blk = 0U;
    rc = ext2_balloc(fs, pref_grp, dir_blk);
    if (rc != kOk) {
        (void)ext2_ifree(fs, new_ino, true);
        return rc;
    }

    // Populate initial "." and ".." entries in the new directory block
    uint8_t block_buf[kExt2MaxBlockSize]{};
    const uint16_t dot_len = dirent_min_len(1U);
    Ext2DirEntry2 dot_ent{
        .inode = new_ino,
        .rec_len = dot_len,
        .name_len = 1U,
        .file_type = kExt2FtDir,
    };
    std::memcpy(block_buf, &dot_ent, sizeof(dot_ent));
    block_buf[sizeof(dot_ent)] = '.';

    Ext2DirEntry2 dotdot_ent{
        .inode = parent_ino,
        .rec_len = static_cast<uint16_t>(fs->block_size - dot_len),
        .name_len = 2U,
        .file_type = kExt2FtDir,
    };
    std::memcpy(block_buf + dot_len, &dotdot_ent, sizeof(dotdot_ent));
    block_buf[dot_len + sizeof(dotdot_ent)] = '.';
    block_buf[dot_len + sizeof(dotdot_ent) + 1U] = '.';

    rc = write_block(fs, dir_blk, block_buf);
    if (rc != kOk) {
        (void)ext2_bfree(fs, dir_blk);
        (void)ext2_ifree(fs, new_ino, true);
        return rc;
    }

    Ext2DiskInode new_disk{};
    new_disk.i_mode = static_cast<uint16_t>(kModeDir | (mode & 07777U));
    new_disk.i_size = fs->block_size;
    new_disk.i_links_count = 2U;
    new_disk.i_blocks = inode_sectors_per_block(fs);
    new_disk.i_block[0] = dir_blk;

    rc = write_disk_inode(fs, new_ino, new_disk);
    if (rc != kOk) {
        (void)ext2_bfree(fs, dir_blk);
        (void)ext2_ifree(fs, new_ino, true);
        return rc;
    }

    Ext2DiskInode parent_disk{};
    rc = read_disk_inode(fs, parent_ino, parent_disk);
    if (rc != kOk) {
        (void)ext2_bfree(fs, dir_blk);
        (void)ext2_ifree(fs, new_ino, true);
        return rc;
    }

    rc = dir_add_entry(fs, parent_ino, parent_disk, target->name_view(), new_ino, kExt2FtDir);
    if (rc != kOk) {
        (void)ext2_bfree(fs, dir_blk);
        (void)ext2_ifree(fs, new_ino, true);
        return rc;
    }

    ++parent_disk.i_links_count;
    rc = write_disk_inode(fs, parent_ino, parent_disk);
    if (rc != kOk) {
        return rc;
    }

    dir->nlink = parent_disk.i_links_count;
    dir->size = parent_disk.i_size;

    Inode* vfs_ino = load_vfs_inode(dir->sb, new_ino);
    if (vfs_ino == nullptr) {
        return kEnomem;
    }
    target->inode = vfs_ino;
    return kOk;
}

int32_t ext2_unlink(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || target->inode == nullptr || dir->sb == nullptr ||
        dir->sb->fs_private == nullptr) {
        return kEinval;
    }
    auto* const fs = static_cast<Ext2Fs*>(dir->sb->fs_private);
    const uint32_t dir_ino = static_cast<uint32_t>(dir->ino);

    Ext2DiskInode dir_disk{};
    int32_t rc = read_disk_inode(fs, dir_ino, dir_disk);
    if (rc != kOk) {
        return rc;
    }

    rc = dir_remove_entry(fs, dir_ino, dir_disk, target->name_view());
    if (rc != kOk) {
        return rc;
    }

    const uint32_t child_ino = static_cast<uint32_t>(target->inode->ino);
    Ext2DiskInode child_disk{};
    rc = read_disk_inode(fs, child_ino, child_disk);
    if (rc != kOk) {
        return rc;
    }

    if (child_disk.i_links_count > 0U) {
        --child_disk.i_links_count;
    }
    target->inode->nlink = child_disk.i_links_count;
    return write_disk_inode(fs, child_ino, child_disk);
}

int32_t ext2_rmdir(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || target->inode == nullptr || dir->sb == nullptr ||
        dir->sb->fs_private == nullptr) {
        return kEinval;
    }
    auto* const fs = static_cast<Ext2Fs*>(dir->sb->fs_private);
    const uint32_t parent_ino = static_cast<uint32_t>(dir->ino);
    const uint32_t child_ino = static_cast<uint32_t>(target->inode->ino);

    Ext2DiskInode child_disk{};
    int32_t rc = read_disk_inode(fs, child_ino, child_disk);
    if (rc != kOk) {
        return rc;
    }

    bool empty = false;
    rc = dir_is_empty(fs, child_ino, child_disk, empty);
    if (rc != kOk) {
        return rc;
    }
    if (!empty) {
        return kEnotempty;
    }

    Ext2DiskInode parent_disk{};
    rc = read_disk_inode(fs, parent_ino, parent_disk);
    if (rc != kOk) {
        return rc;
    }

    rc = dir_remove_entry(fs, parent_ino, parent_disk, target->name_view());
    if (rc != kOk) {
        return rc;
    }

    if (parent_disk.i_links_count > 0U) {
        --parent_disk.i_links_count;
    }
    rc = write_disk_inode(fs, parent_ino, parent_disk);
    if (rc != kOk) {
        return rc;
    }
    dir->nlink = parent_disk.i_links_count;

    child_disk.i_links_count = 0U;
    target->inode->nlink = 0U;
    return write_disk_inode(fs, child_ino, child_disk);
}

int32_t ext2_rename(Inode* old_dir, Dentry* old_dentry, Inode* new_dir,
                    Dentry* new_dentry) noexcept {
    if (old_dir == nullptr || old_dentry == nullptr || old_dentry->inode == nullptr ||
        new_dir == nullptr || new_dentry == nullptr || old_dir->sb == nullptr ||
        old_dir->sb->fs_private == nullptr) {
        return kEinval;
    }
    auto* const fs = static_cast<Ext2Fs*>(old_dir->sb->fs_private);
    const uint32_t old_dir_ino = static_cast<uint32_t>(old_dir->ino);
    const uint32_t new_dir_ino = static_cast<uint32_t>(new_dir->ino);
    const uint32_t src_ino = static_cast<uint32_t>(old_dentry->inode->ino);

    Ext2DiskInode new_dir_disk{};
    int32_t rc = read_disk_inode(fs, new_dir_ino, new_dir_disk);
    if (rc != kOk) {
        return rc;
    }

    // If destination already exists, validate and remove it first
    if (new_dentry->inode != nullptr) {
        const uint32_t dst_ino = static_cast<uint32_t>(new_dentry->inode->ino);
        Ext2DiskInode dst_disk{};
        rc = read_disk_inode(fs, dst_ino, dst_disk);
        if (rc != kOk) {
            return rc;
        }
        if (new_dentry->inode->is_dir()) {
            bool empty = false;
            rc = dir_is_empty(fs, dst_ino, dst_disk, empty);
            if (rc != kOk) {
                return rc;
            }
            if (!empty) {
                return kEnotempty;
            }
            if (new_dir_disk.i_links_count > 0U) {
                --new_dir_disk.i_links_count;
            }
            dst_disk.i_links_count = 0U;
        } else if (dst_disk.i_links_count > 0U) {
            --dst_disk.i_links_count;
        }

        rc = dir_remove_entry(fs, new_dir_ino, new_dir_disk, new_dentry->name_view());
        if (rc != kOk) {
            return rc;
        }
        new_dentry->inode->nlink = dst_disk.i_links_count;
        rc = write_disk_inode(fs, dst_ino, dst_disk);
        if (rc != kOk) {
            return rc;
        }
    }

    const uint8_t ft = mode_to_ext2_ft(old_dentry->inode->mode);
    rc = dir_add_entry(fs, new_dir_ino, new_dir_disk, new_dentry->name_view(), src_ino, ft);
    if (rc != kOk) {
        return rc;
    }

    Ext2DiskInode old_dir_disk{};
    rc = read_disk_inode(fs, old_dir_ino, old_dir_disk);
    if (rc != kOk) {
        return rc;
    }

    rc = dir_remove_entry(fs, old_dir_ino, old_dir_disk, old_dentry->name_view());
    if (rc != kOk) {
        return rc;
    }

    if (old_dentry->inode->is_dir() && old_dir_ino != new_dir_ino) {
        Ext2DiskInode src_disk{};
        rc = read_disk_inode(fs, src_ino, src_disk);
        if (rc != kOk) {
            return rc;
        }
        rc = dir_update_dotdot(fs, src_disk, new_dir_ino);
        if (rc != kOk) {
            return rc;
        }
        if (old_dir_disk.i_links_count > 0U) {
            --old_dir_disk.i_links_count;
        }
        ++new_dir_disk.i_links_count;
        rc = write_disk_inode(fs, old_dir_ino, old_dir_disk);
        if (rc != kOk) {
            return rc;
        }
        old_dir->nlink = old_dir_disk.i_links_count;
    }

    rc = write_disk_inode(fs, new_dir_ino, new_dir_disk);
    if (rc != kOk) {
        return rc;
    }
    new_dir->nlink = new_dir_disk.i_links_count;
    new_dir->size = new_dir_disk.i_size;
    return kOk;
}

int32_t ext2_truncate(Inode* inode, uint64_t new_size) noexcept {
    if (inode == nullptr || inode->sb == nullptr || inode->sb->fs_private == nullptr) {
        return kEinval;
    }
    if (inode->is_dir()) {
        return kEisdir;
    }
    if (new_size > UINT32_MAX) {
        return kEnospc;
    }

    auto* const fs = static_cast<Ext2Fs*>(inode->sb->fs_private);
    const uint32_t ino = static_cast<uint32_t>(inode->ino);

    Ext2DiskInode disk{};
    int32_t rc = read_disk_inode(fs, ino, disk);
    if (rc != kOk) {
        return rc;
    }

    if (new_size == 0U) {
        free_all_inode_blocks(fs, disk);
    } else if (new_size < disk.i_size) {
        const uint32_t keep_blocks =
            static_cast<uint32_t>((new_size + fs->block_size - 1U) / fs->block_size);
        const uint32_t old_blocks =
            static_cast<uint32_t>((disk.i_size + fs->block_size - 1U) / fs->block_size);

        // Zero trailing bytes in the last retained block
        const size_t tail_off = static_cast<size_t>(new_size % fs->block_size);
        if (tail_off != 0U && keep_blocks > 0U) {
            uint32_t last_pblk = 0U;
            bool unused_mod = false;
            if (get_or_alloc_block(fs, ino, disk, keep_blocks - 1U, false, last_pblk, unused_mod) ==
                    kOk &&
                last_pblk != 0U) {
                uint8_t block_buf[kExt2MaxBlockSize]{};
                if (read_block(fs, last_pblk, block_buf) == kOk) {
                    std::memset(block_buf + tail_off, 0, fs->block_size - tail_off);
                    (void)write_block(fs, last_pblk, block_buf);
                }
            }
        }

        // Release direct blocks beyond keep_blocks
        for (uint32_t blk = keep_blocks; blk < old_blocks && blk < kDirectBlocks; ++blk) {
            if (disk.i_block[blk] != 0U) {
                (void)ext2_bfree(fs, disk.i_block[blk]);
                disk.i_block[blk] = 0U;
                const uint32_t spb = inode_sectors_per_block(fs);
                if (disk.i_blocks >= spb) {
                    disk.i_blocks -= spb;
                }
            }
        }
    }

    disk.i_size = static_cast<uint32_t>(new_size);
    rc = write_disk_inode(fs, ino, disk);
    if (rc != kOk) {
        return rc;
    }

    inode->size = new_size;
    return kOk;
}

int64_t ext2_read(File* file, void* buf, size_t count, uint64_t* offset) noexcept {
    if (file == nullptr || file->inode == nullptr || file->inode->sb == nullptr ||
        file->inode->sb->fs_private == nullptr || offset == nullptr) {
        return kEinval;
    }
    if (file->inode->is_dir()) {
        return kEisdir;
    }
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kEinval;
    }

    auto* const fs = static_cast<Ext2Fs*>(file->inode->sb->fs_private);
    const uint32_t ino = static_cast<uint32_t>(file->inode->ino);

    Ext2DiskInode disk{};
    const int32_t rrc = read_disk_inode(fs, ino, disk);
    if (rrc != kOk) {
        return rrc;
    }

    if (*offset >= disk.i_size) {
        return 0;
    }

    const uint64_t avail = disk.i_size - *offset;
    const size_t to_read =
        (static_cast<uint64_t>(count) < avail) ? count : static_cast<size_t>(avail);

    auto* dst = static_cast<uint8_t*>(buf);
    uint8_t block_buf[kExt2MaxBlockSize]{};
    size_t total_read = 0U;
    bool unused_mod = false;

    while (total_read < to_read) {
        const uint32_t lblk = static_cast<uint32_t>(*offset / fs->block_size);
        const size_t boff = static_cast<size_t>(*offset % fs->block_size);
        const size_t brem = fs->block_size - boff;
        const size_t rem = to_read - total_read;
        const size_t chunk = (rem < brem) ? rem : brem;

        uint32_t pblk = 0U;
        const int32_t brc = get_or_alloc_block(fs, ino, disk, lblk, false, pblk, unused_mod);
        if (brc != kOk) {
            return (total_read > 0U) ? static_cast<int64_t>(total_read) : brc;
        }

        if (pblk == 0U) {
            std::memset(dst + total_read, 0, chunk);
        } else {
            const int32_t rc = read_block(fs, pblk, block_buf);
            if (rc != kOk) {
                return (total_read > 0U) ? static_cast<int64_t>(total_read) : rc;
            }
            std::memcpy(dst + total_read, block_buf + boff, chunk);
        }

        *offset += chunk;
        total_read += chunk;
    }

    return static_cast<int64_t>(total_read);
}

int64_t ext2_write(File* file, const void* buf, size_t count, uint64_t* offset) noexcept {
    if (file == nullptr || file->inode == nullptr || file->inode->sb == nullptr ||
        file->inode->sb->fs_private == nullptr || offset == nullptr) {
        return kEinval;
    }
    if (file->inode->is_dir()) {
        return kEisdir;
    }
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kEinval;
    }

    auto* const fs = static_cast<Ext2Fs*>(file->inode->sb->fs_private);
    const uint32_t ino = static_cast<uint32_t>(file->inode->ino);

    Ext2DiskInode disk{};
    const int32_t rrc = read_disk_inode(fs, ino, disk);
    if (rrc != kOk) {
        return rrc;
    }

    const auto* src = static_cast<const uint8_t*>(buf);
    uint8_t block_buf[kExt2MaxBlockSize]{};
    size_t total_written = 0U;
    bool inode_mod = false;

    while (total_written < count) {
        const uint32_t lblk = static_cast<uint32_t>(*offset / fs->block_size);
        const size_t boff = static_cast<size_t>(*offset % fs->block_size);
        const size_t brem = fs->block_size - boff;
        const size_t rem = count - total_written;
        const size_t chunk = (rem < brem) ? rem : brem;

        uint32_t pblk = 0U;
        const int32_t brc = get_or_alloc_block(fs, ino, disk, lblk, true, pblk, inode_mod);
        if (brc != kOk) {
            if (inode_mod) {
                (void)write_disk_inode(fs, ino, disk);
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : brc;
        }

        if (chunk < fs->block_size) {
            const int32_t rc = read_block(fs, pblk, block_buf);
            if (rc != kOk) {
                if (inode_mod) {
                    (void)write_disk_inode(fs, ino, disk);
                }
                return (total_written > 0U) ? static_cast<int64_t>(total_written) : rc;
            }
        }

        std::memcpy(block_buf + boff, src + total_written, chunk);
        const int32_t wrc = write_block(fs, pblk, block_buf);
        if (wrc != kOk) {
            if (inode_mod) {
                (void)write_disk_inode(fs, ino, disk);
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : wrc;
        }

        *offset += chunk;
        total_written += chunk;
    }

    if (*offset > disk.i_size) {
        disk.i_size = static_cast<uint32_t>(*offset);
        file->inode->size = *offset;
        inode_mod = true;
    }

    if (inode_mod) {
        const int32_t irc = write_disk_inode(fs, ino, disk);
        if (irc != kOk) {
            return irc;
        }
    }

    return static_cast<int64_t>(total_written);
}

int32_t ext2_fsync(File* file, int32_t /*datasync*/) noexcept {
    if (file == nullptr || file->inode == nullptr || file->inode->sb == nullptr ||
        file->inode->sb->fs_private == nullptr) {
        return kEbadf;
    }
    return sync_metadata(static_cast<Ext2Fs*>(file->inode->sb->fs_private));
}

int32_t ext2_readdir(File* file, DirEntryInfo* out_entry) noexcept {
    if (file == nullptr || file->inode == nullptr || file->inode->sb == nullptr ||
        file->inode->sb->fs_private == nullptr || out_entry == nullptr) {
        return kEinval;
    }

    auto* const fs = static_cast<Ext2Fs*>(file->inode->sb->fs_private);
    const uint32_t dir_ino = static_cast<uint32_t>(file->inode->ino);

    Ext2DiskInode dir_disk{};
    const int32_t rrc = read_disk_inode(fs, dir_ino, dir_disk);
    if (rrc != kOk) {
        return rrc;
    }

    const uint32_t total_blocks =
        static_cast<uint32_t>((dir_disk.i_size + fs->block_size - 1U) / fs->block_size);

    uint8_t block_buf[kExt2MaxBlockSize]{};
    uint64_t logical_idx = 0U;
    bool unused_mod = false;

    for (uint32_t lblk = 0U; lblk < total_blocks; ++lblk) {
        uint32_t pblk = 0U;
        const int32_t brc =
            get_or_alloc_block(fs, dir_ino, dir_disk, lblk, false, pblk, unused_mod);
        if (brc != kOk) {
            return brc;
        }
        if (pblk == 0U) {
            continue;
        }
        const int32_t rc = read_block(fs, pblk, block_buf);
        if (rc != kOk) {
            return rc;
        }

        size_t off = 0U;
        while (off + sizeof(Ext2DirEntry2) <= fs->block_size) {
            Ext2DirEntry2 ent{};
            std::memcpy(&ent, block_buf + off, sizeof(ent));
            if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > fs->block_size) {
                break;
            }
            if (ent.inode != 0U && ent.name_len > 0U &&
                off + sizeof(Ext2DirEntry2) + ent.name_len <= fs->block_size) {
                const auto* ename = reinterpret_cast<const char*>(block_buf + off + sizeof(ent));
                const StringView sv(ename, ent.name_len);
                // sys_getdents64 synthesizes "." and ".." at offsets 0 and 1
                if (sv != "." && sv != "..") {
                    if (logical_idx == file->offset) {
                        out_entry->ino = ent.inode;
                        out_entry->type = ext2_ft_to_vfs_type(ent.file_type);
                        const size_t copy_len =
                            (ent.name_len < kMaxNameLen) ? ent.name_len : kMaxNameLen;
                        for (size_t i = 0U; i < copy_len; ++i) {
                            out_entry->name[i] = ename[i];
                        }
                        out_entry->name[copy_len] = '\0';
                        ++file->offset;
                        return kOk;
                    }
                    ++logical_idx;
                }
            }
            off += ent.rec_len;
        }
    }

    return kEnoent;
}

const SuperblockOperations kExt2SbOps{
    .sync_fs = ext2_sync_fs,
    .alloc_inode = nullptr,
    .destroy_inode = ext2_destroy_inode,
};

const InodeOperations kExt2InodeOps{
    .lookup = ext2_lookup,
    .create = ext2_create,
    .mkdir = ext2_mkdir,
    .unlink = ext2_unlink,
    .rmdir = ext2_rmdir,
    .rename = ext2_rename,
    .truncate = ext2_truncate,
    .readlink = nullptr,
};

const FileOperations kExt2FileOps{
    .read = ext2_read,
    .write = ext2_write,
    .open = nullptr,
    .release = nullptr,
    .lseek = nullptr,
    .ioctl = nullptr,
    .fsync = ext2_fsync,
    .readdir = ext2_readdir,
    .poll = nullptr,
};

void release_dentry_subtree(Dentry* node) noexcept {
    if (node == nullptr) {
        return;
    }
    Dentry* child = node->first_child;
    while (child != nullptr) {
        Dentry* next = child->next_sibling;
        dentry_detach(child);
        release_dentry_subtree(child);
        child = next;
    }
    dentry_put(node);
}

} // namespace

void ext2_reset() noexcept {
    for (auto& slot : g_ext2_fs) {
        slot = Ext2Fs{};
    }
}

int32_t ext2_create_sb(Kernel::Drivers::Block::BlockDevice* dev, Superblock** out_sb) noexcept {
    if (dev == nullptr || out_sb == nullptr) {
        return kEinval;
    }
    *out_sb = nullptr;

    const size_t sec_size = dev->sector_size();
    if (sec_size == 0U || (kExt2SuperblockOffset % sec_size) != 0U ||
        (sizeof(Ext2Superblock) % sec_size) != 0U) {
        return kEinval;
    }

    Ext2Fs* fs = alloc_ext2_fs();
    if (fs == nullptr) {
        return kEnomem;
    }
    fs->dev = dev;

    // 1. Read and validate superblock at byte offset 1024
    const uint64_t sb_lba = kExt2SuperblockOffset / sec_size;
    const size_t sb_secs = sizeof(Ext2Superblock) / sec_size;
    if (dev->read_sectors(sb_lba, sb_secs, &fs->sb) != 0) {
        free_ext2_fs(fs);
        return kEio;
    }

    if (fs->sb.s_magic != kExt2SuperMagic || fs->sb.s_log_block_size > 2U ||
        fs->sb.s_blocks_per_group == 0U || fs->sb.s_inodes_per_group == 0U ||
        fs->sb.s_blocks_count == 0U || fs->sb.s_inodes_count == 0U) {
        free_ext2_fs(fs);
        return kEinval;
    }

    fs->block_size = 1024U << fs->sb.s_log_block_size;
    fs->ptrs_per_block = static_cast<uint32_t>(fs->block_size / sizeof(uint32_t));
    fs->inode_size =
        (fs->sb.s_rev_level >= 1U && fs->sb.s_inode_size >= 128U) ? fs->sb.s_inode_size : 128U;
    fs->group_count =
        (fs->sb.s_blocks_count + fs->sb.s_blocks_per_group - 1U) / fs->sb.s_blocks_per_group;
    if (fs->group_count == 0U || fs->group_count > kMaxBlockGroups) {
        free_ext2_fs(fs);
        return kEinval;
    }

    // BGDT immediately follows the superblock block (block 2 for 1 KiB, block 1 for 2/4 KiB)
    fs->bgdt_start_block = (fs->block_size == 1024U) ? 2U : 1U;

    // 2. Read Block Group Descriptor Table (BGDT)
    const size_t bgdt_bytes = static_cast<size_t>(fs->group_count) * sizeof(Ext2BlockGroupDesc);
    const uint32_t bgdt_blocks =
        static_cast<uint32_t>((bgdt_bytes + fs->block_size - 1U) / fs->block_size);

    uint8_t block_buf[kExt2MaxBlockSize]{};
    auto* raw_bgdt = reinterpret_cast<uint8_t*>(fs->bgdt);
    for (uint32_t i = 0U; i < bgdt_blocks; ++i) {
        const int32_t rc = read_block(fs, fs->bgdt_start_block + i, block_buf);
        if (rc != kOk) {
            free_ext2_fs(fs);
            return rc;
        }
        const size_t off = static_cast<size_t>(i) * fs->block_size;
        const size_t rem = bgdt_bytes - off;
        const size_t chunk = (rem < fs->block_size) ? rem : fs->block_size;
        std::memcpy(raw_bgdt + off, block_buf, chunk);
    }

    // 3. Allocate VFS Superblock & Root Inode (ino == 2)
    Superblock* superblock = vfs_alloc_superblock();
    if (superblock == nullptr) {
        free_ext2_fs(fs);
        return kEnomem;
    }
    superblock->fs_type = "ext2";
    superblock->block_size = fs->block_size;
    superblock->flags = 0U;
    superblock->ops = &kExt2SbOps;
    superblock->fs_private = fs;

    Inode* root_inode = load_vfs_inode(superblock, kExt2RootIno);
    if (root_inode == nullptr || !root_inode->is_dir()) {
        if (root_inode != nullptr) {
            inode_put(root_inode);
        }
        vfs_free_superblock(superblock);
        free_ext2_fs(fs);
        return kEinval;
    }

    Dentry* root_dentry = vfs_alloc_dentry(superblock, nullptr, "ext2");
    if (root_dentry == nullptr) {
        inode_put(root_inode);
        vfs_free_superblock(superblock);
        free_ext2_fs(fs);
        return kEnomem;
    }
    root_dentry->inode = root_inode;
    superblock->root = root_dentry;

    *out_sb = superblock;
    return kOk;
}

int32_t ext2_mount(Kernel::Drivers::Block::BlockDevice* dev, StringView mount_path,
                   Superblock** out_sb) noexcept {
    Superblock* sb = nullptr;
    const int32_t crc = ext2_create_sb(dev, &sb);
    if (crc != kOk) {
        return crc;
    }

    const int32_t mrc = vfs_mount(mount_path, sb);
    if (mrc != kOk) {
        release_dentry_subtree(sb->root);
        free_ext2_fs(static_cast<Ext2Fs*>(sb->fs_private));
        vfs_free_superblock(sb);
        return mrc;
    }

    if (out_sb != nullptr) {
        *out_sb = sb;
    }
    return kOk;
}

int32_t ext2_umount(StringView mount_path, Superblock* sb) noexcept {
    const int32_t urc = vfs_umount(mount_path);
    if (urc != kOk) {
        return urc;
    }
    if (sb != nullptr) {
        release_dentry_subtree(sb->root);
        free_ext2_fs(static_cast<Ext2Fs*>(sb->fs_private));
        vfs_free_superblock(sb);
    }
    return kOk;
}

} // namespace kernel::fs