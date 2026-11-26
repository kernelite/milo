#include "fs/ext2/ext2.hpp"

#include <cstring>

namespace Kernel::FS {

namespace {

constexpr size_t kMaxExt2Mounts = 4U;
constexpr size_t kMaxBlockGroups = 128U;
constexpr uint32_t kDirectBlocks = 12U;
constexpr uint32_t kSinglyIndirectIdx = 12U;
constexpr uint32_t kDoublyIndirectIdx = 13U;

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

class Ext2Filesystem final {
  public:
    constexpr Ext2Filesystem() noexcept = default;
    ~Ext2Filesystem() noexcept = default;

    Ext2Filesystem(const Ext2Filesystem&) = delete;
    Ext2Filesystem& operator=(const Ext2Filesystem&) = delete;
    Ext2Filesystem(Ext2Filesystem&&) = delete;
    Ext2Filesystem& operator=(Ext2Filesystem&&) = delete;

    [[nodiscard]] bool is_active() const noexcept { return m_active; }
    [[nodiscard]] uint32_t block_size() const noexcept { return m_block_size; }

    void reset() noexcept {
        m_dev = nullptr;
        m_sb = Ext2Superblock{};
        for (auto& desc : m_bgdt) {
            desc = Ext2BlockGroupDesc{};
        }
        m_block_size = 1024U;
        m_inode_size = 128U;
        m_group_count = 0U;
        m_ptrs_per_block = 256U;
        m_bgdt_start_block = 2U;
        m_sb_dirty = false;
        m_bgdt_dirty = false;
        m_active = false;
    }

    [[nodiscard]] int32_t mount_device(Kernel::Drivers::Block::BlockDevice* dev) noexcept {
        if (dev == nullptr) {
            return kEinval;
        }
        const size_t sec_size = dev->sector_size();
        if (sec_size == 0U || (kExt2SuperblockOffset % sec_size) != 0U ||
            (sizeof(Ext2Superblock) % sec_size) != 0U) {
            return kEinval;
        }

        reset();
        m_dev = dev;
        m_active = true;

        const uint64_t sb_lba = kExt2SuperblockOffset / sec_size;
        const size_t sb_secs = sizeof(Ext2Superblock) / sec_size;
        if (m_dev->read_sectors(sb_lba, sb_secs, &m_sb) != 0) {
            reset();
            return kEio;
        }

        if (m_sb.s_magic != kExt2SuperMagic || m_sb.s_log_block_size > 2U ||
            m_sb.s_blocks_per_group == 0U || m_sb.s_inodes_per_group == 0U ||
            m_sb.s_blocks_count == 0U || m_sb.s_inodes_count == 0U) {
            reset();
            return kEinval;
        }

        m_block_size = 1024U << m_sb.s_log_block_size;
        m_ptrs_per_block = static_cast<uint32_t>(m_block_size / sizeof(uint32_t));
        m_inode_size =
            (m_sb.s_rev_level >= 1U && m_sb.s_inode_size >= 128U) ? m_sb.s_inode_size : 128U;
        m_group_count =
            (m_sb.s_blocks_count + m_sb.s_blocks_per_group - 1U) / m_sb.s_blocks_per_group;
        if (m_group_count == 0U || m_group_count > kMaxBlockGroups) {
            reset();
            return kEinval;
        }

        m_bgdt_start_block = (m_block_size == 1024U) ? 2U : 1U;

        const size_t bgdt_bytes = static_cast<size_t>(m_group_count) * sizeof(Ext2BlockGroupDesc);
        const uint32_t bgdt_blocks =
            static_cast<uint32_t>((bgdt_bytes + m_block_size - 1U) / m_block_size);

        uint8_t block_buf[kExt2MaxBlockSize]{};
        auto* const raw_bgdt = reinterpret_cast<uint8_t*>(m_bgdt);
        for (uint32_t i = 0U; i < bgdt_blocks; ++i) {
            const int32_t rc = read_block(m_bgdt_start_block + i, block_buf);
            if (rc != kOk) {
                reset();
                return rc;
            }
            const size_t off = static_cast<size_t>(i) * m_block_size;
            const size_t rem = bgdt_bytes - off;
            const size_t chunk = (rem < m_block_size) ? rem : m_block_size;
            std::memcpy(raw_bgdt + off, block_buf, chunk);
        }

        return kOk;
    }

    [[nodiscard]] int32_t sync_metadata() noexcept {
        if (!m_active) {
            return kEinval;
        }
        if (m_bgdt_dirty) {
            const int32_t rc = write_bgdt();
            if (rc != kOk) {
                return rc;
            }
        }
        if (m_sb_dirty) {
            const int32_t rc = write_superblock();
            if (rc != kOk) {
                return rc;
            }
        }
        return kOk;
    }

    [[nodiscard]] uint32_t preferred_group_for_ino(uint32_t ino) const noexcept {
        return (ino > 0U) ? ((ino - 1U) / m_sb.s_inodes_per_group) : 0U;
    }

    [[nodiscard]] int32_t read_block(uint32_t block_no, void* dst) const noexcept {
        if (!m_active || m_dev == nullptr || dst == nullptr || block_no >= m_sb.s_blocks_count) {
            return kEio;
        }
        const size_t sec_size = m_dev->sector_size();
        if (sec_size == 0U || (m_block_size % sec_size) != 0U) {
            return kEio;
        }
        const size_t sectors_per_block = m_block_size / sec_size;
        const uint64_t start_lba = static_cast<uint64_t>(block_no) * sectors_per_block;
        return (m_dev->read_sectors(start_lba, sectors_per_block, dst) == 0) ? kOk : kEio;
    }

    [[nodiscard]] int32_t write_block(uint32_t block_no, const void* src) const noexcept {
        if (!m_active || m_dev == nullptr || src == nullptr || block_no >= m_sb.s_blocks_count) {
            return kEio;
        }
        const size_t sec_size = m_dev->sector_size();
        if (sec_size == 0U || (m_block_size % sec_size) != 0U) {
            return kEio;
        }
        const size_t sectors_per_block = m_block_size / sec_size;
        const uint64_t start_lba = static_cast<uint64_t>(block_no) * sectors_per_block;
        return (m_dev->write_sectors(start_lba, sectors_per_block, src) == 0) ? kOk : kEio;
    }

    [[nodiscard]] int32_t alloc_inode(uint32_t preferred_group, bool is_dir,
                                      uint32_t& out_ino) noexcept {
        out_ino = 0U;
        if (!m_active || m_sb.s_free_inodes_count == 0U) {
            return kEnospc;
        }

        uint8_t bitmap[kExt2MaxBlockSize]{};
        const uint32_t start_group = (preferred_group < m_group_count) ? preferred_group : 0U;

        for (uint32_t offset = 0U; offset < m_group_count; ++offset) {
            const uint32_t grp = (start_group + offset) % m_group_count;
            if (m_bgdt[grp].bg_free_inodes_count == 0U) {
                continue;
            }

            const int32_t rrc = read_block(m_bgdt[grp].bg_inode_bitmap, bitmap);
            if (rrc != kOk) {
                return rrc;
            }

            for (uint32_t bit = 0U; bit < m_sb.s_inodes_per_group; ++bit) {
                const uint32_t candidate_ino = (grp * m_sb.s_inodes_per_group) + bit + 1U;
                if (candidate_ino > m_sb.s_inodes_count) {
                    break;
                }
                const uint32_t first_ino =
                    (m_sb.s_rev_level >= 1U && m_sb.s_first_ino > 0U) ? m_sb.s_first_ino : 11U;
                if (candidate_ino < first_ino) {
                    continue;
                }

                const uint32_t byte_idx = bit / 8U;
                const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8U));
                if ((bitmap[byte_idx] & mask) == 0U) {
                    bitmap[byte_idx] |= mask;
                    const int32_t wrc = write_block(m_bgdt[grp].bg_inode_bitmap, bitmap);
                    if (wrc != kOk) {
                        return wrc;
                    }

                    --m_bgdt[grp].bg_free_inodes_count;
                    if (is_dir) {
                        ++m_bgdt[grp].bg_used_dirs_count;
                    }
                    --m_sb.s_free_inodes_count;
                    m_bgdt_dirty = true;
                    m_sb_dirty = true;

                    out_ino = candidate_ino;
                    return kOk;
                }
            }
        }

        return kEnospc;
    }

    [[nodiscard]] int32_t free_inode(uint32_t ino, bool is_dir) noexcept {
        if (!m_active || ino == 0U || ino > m_sb.s_inodes_count) {
            return kEinval;
        }

        const uint32_t idx = ino - 1U;
        const uint32_t grp = idx / m_sb.s_inodes_per_group;
        const uint32_t bit = idx % m_sb.s_inodes_per_group;
        if (grp >= m_group_count) {
            return kEinval;
        }

        uint8_t bitmap[kExt2MaxBlockSize]{};
        const int32_t rrc = read_block(m_bgdt[grp].bg_inode_bitmap, bitmap);
        if (rrc != kOk) {
            return rrc;
        }

        const uint32_t byte_idx = bit / 8U;
        const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8U));
        if ((bitmap[byte_idx] & mask) == 0U) {
            return kOk;
        }

        bitmap[byte_idx] = static_cast<uint8_t>(bitmap[byte_idx] & ~static_cast<uint32_t>(mask));
        const int32_t wrc = write_block(m_bgdt[grp].bg_inode_bitmap, bitmap);
        if (wrc != kOk) {
            return wrc;
        }

        ++m_bgdt[grp].bg_free_inodes_count;
        if (is_dir && m_bgdt[grp].bg_used_dirs_count > 0U) {
            --m_bgdt[grp].bg_used_dirs_count;
        }
        ++m_sb.s_free_inodes_count;
        m_bgdt_dirty = true;
        m_sb_dirty = true;
        return kOk;
    }

    [[nodiscard]] int32_t read_disk_inode(uint32_t ino, Ext2DiskInode& out_inode) const noexcept {
        if (!m_active || ino == 0U || ino > m_sb.s_inodes_count) {
            return kEinval;
        }
        const uint32_t idx = ino - 1U;
        const uint32_t grp = idx / m_sb.s_inodes_per_group;
        const uint32_t local_idx = idx % m_sb.s_inodes_per_group;
        if (grp >= m_group_count) {
            return kEinval;
        }

        const uint64_t byte_offset = static_cast<uint64_t>(local_idx) * m_inode_size;
        const uint32_t block_no =
            m_bgdt[grp].bg_inode_table + static_cast<uint32_t>(byte_offset / m_block_size);
        const size_t offset_in_block = static_cast<size_t>(byte_offset % m_block_size);

        uint8_t block_buf[kExt2MaxBlockSize]{};
        const int32_t rc = read_block(block_no, block_buf);
        if (rc != kOk) {
            return rc;
        }

        std::memcpy(&out_inode, block_buf + offset_in_block, sizeof(Ext2DiskInode));
        return kOk;
    }

    [[nodiscard]] int32_t write_disk_inode(uint32_t ino,
                                           const Ext2DiskInode& disk_inode) const noexcept {
        if (!m_active || ino == 0U || ino > m_sb.s_inodes_count) {
            return kEinval;
        }
        const uint32_t idx = ino - 1U;
        const uint32_t grp = idx / m_sb.s_inodes_per_group;
        const uint32_t local_idx = idx % m_sb.s_inodes_per_group;
        if (grp >= m_group_count) {
            return kEinval;
        }

        const uint64_t byte_offset = static_cast<uint64_t>(local_idx) * m_inode_size;
        const uint32_t block_no =
            m_bgdt[grp].bg_inode_table + static_cast<uint32_t>(byte_offset / m_block_size);
        const size_t offset_in_block = static_cast<size_t>(byte_offset % m_block_size);

        uint8_t block_buf[kExt2MaxBlockSize]{};
        const int32_t rrc = read_block(block_no, block_buf);
        if (rrc != kOk) {
            return rrc;
        }

        std::memcpy(block_buf + offset_in_block, &disk_inode, sizeof(Ext2DiskInode));
        return write_block(block_no, block_buf);
    }

    [[nodiscard]] int32_t get_or_alloc_block(uint32_t ino, Ext2DiskInode& disk_inode,
                                             uint32_t logical_blk, bool allocate,
                                             uint32_t& out_phys_blk,
                                             bool& inode_modified) noexcept {
        out_phys_blk = 0U;
        const uint32_t pref_grp = preferred_group_for_ino(ino);
        const uint32_t ppb = m_ptrs_per_block;

        if (logical_blk < kDirectBlocks) {
            if (disk_inode.i_block[logical_blk] == 0U) {
                if (!allocate) {
                    return kOk;
                }
                uint32_t new_blk = 0U;
                const int32_t rc = alloc_block(pref_grp, new_blk);
                if (rc != kOk) {
                    return rc;
                }
                disk_inode.i_block[logical_blk] = new_blk;
                disk_inode.i_blocks += sectors_per_block();
                inode_modified = true;
            }
            out_phys_blk = disk_inode.i_block[logical_blk];
            return kOk;
        }

        uint32_t rel = logical_blk - kDirectBlocks;
        if (rel < ppb) {
            if (disk_inode.i_block[kSinglyIndirectIdx] == 0U) {
                if (!allocate) {
                    return kOk;
                }
                uint32_t ind_blk = 0U;
                const int32_t rc = alloc_block(pref_grp, ind_blk);
                if (rc != kOk) {
                    return rc;
                }
                disk_inode.i_block[kSinglyIndirectIdx] = ind_blk;
                disk_inode.i_blocks += sectors_per_block();
                inode_modified = true;
            }

            uint32_t ind_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
            const int32_t rrc = read_block(disk_inode.i_block[kSinglyIndirectIdx], ind_table);
            if (rrc != kOk) {
                return rrc;
            }

            if (ind_table[rel] == 0U) {
                if (!allocate) {
                    return kOk;
                }
                uint32_t new_blk = 0U;
                const int32_t rc = alloc_block(pref_grp, new_blk);
                if (rc != kOk) {
                    return rc;
                }
                ind_table[rel] = new_blk;
                const int32_t wrc = write_block(disk_inode.i_block[kSinglyIndirectIdx], ind_table);
                if (wrc != kOk) {
                    return wrc;
                }
                disk_inode.i_blocks += sectors_per_block();
                inode_modified = true;
            }

            out_phys_blk = ind_table[rel];
            return kOk;
        }

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
                const int32_t rc = alloc_block(pref_grp, dbl_blk);
                if (rc != kOk) {
                    return rc;
                }
                disk_inode.i_block[kDoublyIndirectIdx] = dbl_blk;
                disk_inode.i_blocks += sectors_per_block();
                inode_modified = true;
            }

            uint32_t l1_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
            int32_t rc = read_block(disk_inode.i_block[kDoublyIndirectIdx], l1_table);
            if (rc != kOk) {
                return rc;
            }

            if (l1_table[l1_idx] == 0U) {
                if (!allocate) {
                    return kOk;
                }
                uint32_t l2_blk = 0U;
                rc = alloc_block(pref_grp, l2_blk);
                if (rc != kOk) {
                    return rc;
                }
                l1_table[l1_idx] = l2_blk;
                rc = write_block(disk_inode.i_block[kDoublyIndirectIdx], l1_table);
                if (rc != kOk) {
                    return rc;
                }
                disk_inode.i_blocks += sectors_per_block();
                inode_modified = true;
            }

            uint32_t l2_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
            rc = read_block(l1_table[l1_idx], l2_table);
            if (rc != kOk) {
                return rc;
            }

            if (l2_table[l2_idx] == 0U) {
                if (!allocate) {
                    return kOk;
                }
                uint32_t new_blk = 0U;
                rc = alloc_block(pref_grp, new_blk);
                if (rc != kOk) {
                    return rc;
                }
                l2_table[l2_idx] = new_blk;
                rc = write_block(l1_table[l1_idx], l2_table);
                if (rc != kOk) {
                    return rc;
                }
                disk_inode.i_blocks += sectors_per_block();
                inode_modified = true;
            }

            out_phys_blk = l2_table[l2_idx];
            return kOk;
        }

        return kEnospc;
    }

    void free_all_inode_blocks(Ext2DiskInode& disk_inode) noexcept {
        for (uint32_t i = 0U; i < kDirectBlocks; ++i) {
            if (disk_inode.i_block[i] != 0U) {
                (void)free_block(disk_inode.i_block[i]);
                disk_inode.i_block[i] = 0U;
            }
        }

        if (disk_inode.i_block[kSinglyIndirectIdx] != 0U) {
            uint32_t ind_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
            if (read_block(disk_inode.i_block[kSinglyIndirectIdx], ind_table) == kOk) {
                for (uint32_t i = 0U; i < m_ptrs_per_block; ++i) {
                    if (ind_table[i] != 0U) {
                        (void)free_block(ind_table[i]);
                    }
                }
            }
            (void)free_block(disk_inode.i_block[kSinglyIndirectIdx]);
            disk_inode.i_block[kSinglyIndirectIdx] = 0U;
        }

        if (disk_inode.i_block[kDoublyIndirectIdx] != 0U) {
            uint32_t l1_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
            if (read_block(disk_inode.i_block[kDoublyIndirectIdx], l1_table) == kOk) {
                uint32_t l2_table[kExt2MaxBlockSize / sizeof(uint32_t)]{};
                for (uint32_t i = 0U; i < m_ptrs_per_block; ++i) {
                    if (l1_table[i] != 0U) {
                        if (read_block(l1_table[i], l2_table) == kOk) {
                            for (uint32_t j = 0U; j < m_ptrs_per_block; ++j) {
                                if (l2_table[j] != 0U) {
                                    (void)free_block(l2_table[j]);
                                }
                            }
                        }
                        (void)free_block(l1_table[i]);
                    }
                }
            }
            (void)free_block(disk_inode.i_block[kDoublyIndirectIdx]);
            disk_inode.i_block[kDoublyIndirectIdx] = 0U;
        }

        disk_inode.i_blocks = 0U;
    }

    [[nodiscard]] int32_t dir_find_entry(uint32_t dir_ino, const Ext2DiskInode& dir_disk,
                                         StringView name, uint32_t& out_child_ino) noexcept {
        out_child_ino = 0U;
        const uint32_t total_blocks =
            static_cast<uint32_t>((dir_disk.i_size + m_block_size - 1U) / m_block_size);

        uint8_t block_buf[kExt2MaxBlockSize]{};
        Ext2DiskInode mutable_copy = dir_disk;
        bool unused_mod = false;

        for (uint32_t lblk = 0U; lblk < total_blocks; ++lblk) {
            uint32_t pblk = 0U;
            const int32_t brc =
                get_or_alloc_block(dir_ino, mutable_copy, lblk, false, pblk, unused_mod);
            if (brc != kOk) {
                return brc;
            }
            if (pblk == 0U) {
                continue;
            }
            const int32_t rrc = read_block(pblk, block_buf);
            if (rrc != kOk) {
                return rrc;
            }

            size_t off = 0U;
            while (off + sizeof(Ext2DirEntry2) <= m_block_size) {
                Ext2DirEntry2 ent{};
                std::memcpy(&ent, block_buf + off, sizeof(ent));
                if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > m_block_size) {
                    break;
                }
                if (ent.inode != 0U && ent.name_len == name.size() &&
                    off + sizeof(Ext2DirEntry2) + ent.name_len <= m_block_size) {
                    const auto* const ename =
                        reinterpret_cast<const char*>(block_buf + off + sizeof(ent));
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

    [[nodiscard]] int32_t dir_is_empty(uint32_t dir_ino, const Ext2DiskInode& dir_disk,
                                       bool& out_empty) noexcept {
        out_empty = true;
        const uint32_t total_blocks =
            static_cast<uint32_t>((dir_disk.i_size + m_block_size - 1U) / m_block_size);

        uint8_t block_buf[kExt2MaxBlockSize]{};
        Ext2DiskInode mutable_copy = dir_disk;
        bool unused_mod = false;

        for (uint32_t lblk = 0U; lblk < total_blocks; ++lblk) {
            uint32_t pblk = 0U;
            const int32_t brc =
                get_or_alloc_block(dir_ino, mutable_copy, lblk, false, pblk, unused_mod);
            if (brc != kOk) {
                return brc;
            }
            if (pblk == 0U) {
                continue;
            }
            const int32_t rrc = read_block(pblk, block_buf);
            if (rrc != kOk) {
                return rrc;
            }

            size_t off = 0U;
            while (off + sizeof(Ext2DirEntry2) <= m_block_size) {
                Ext2DirEntry2 ent{};
                std::memcpy(&ent, block_buf + off, sizeof(ent));
                if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > m_block_size) {
                    break;
                }
                if (ent.inode != 0U && off + sizeof(Ext2DirEntry2) + ent.name_len <= m_block_size) {
                    const auto* const ename =
                        reinterpret_cast<const char*>(block_buf + off + sizeof(ent));
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

    [[nodiscard]] int32_t dir_add_entry(uint32_t dir_ino, Ext2DiskInode& dir_disk, StringView name,
                                        uint32_t child_ino, uint8_t file_type) noexcept {
        if (name.empty() || name.size() > 255U) {
            return kEnametoolong;
        }

        const uint16_t needed = dirent_min_len(name.size());
        const uint32_t total_blocks =
            static_cast<uint32_t>((dir_disk.i_size + m_block_size - 1U) / m_block_size);

        uint8_t block_buf[kExt2MaxBlockSize]{};
        bool inode_mod = false;

        for (uint32_t lblk = 0U; lblk < total_blocks; ++lblk) {
            uint32_t pblk = 0U;
            const int32_t brc = get_or_alloc_block(dir_ino, dir_disk, lblk, false, pblk, inode_mod);
            if (brc != kOk) {
                return brc;
            }
            if (pblk == 0U) {
                continue;
            }
            const int32_t rrc = read_block(pblk, block_buf);
            if (rrc != kOk) {
                return rrc;
            }

            size_t off = 0U;
            while (off + sizeof(Ext2DirEntry2) <= m_block_size) {
                Ext2DirEntry2 ent{};
                std::memcpy(&ent, block_buf + off, sizeof(ent));
                if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > m_block_size) {
                    break;
                }

                if (ent.inode == 0U && ent.rec_len >= needed) {
                    ent.inode = child_ino;
                    ent.name_len = static_cast<uint8_t>(name.size());
                    ent.file_type = file_type;
                    std::memcpy(block_buf + off, &ent, sizeof(ent));
                    std::memcpy(block_buf + off + sizeof(ent), name.data(), name.size());
                    return write_block(pblk, block_buf);
                }

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
                    return write_block(pblk, block_buf);
                }

                off += ent.rec_len;
            }
        }

        uint32_t new_pblk = 0U;
        const int32_t brc =
            get_or_alloc_block(dir_ino, dir_disk, total_blocks, true, new_pblk, inode_mod);
        if (brc != kOk) {
            return brc;
        }

        std::memset(block_buf, 0, m_block_size);
        Ext2DirEntry2 new_ent{
            .inode = child_ino,
            .rec_len = static_cast<uint16_t>(m_block_size),
            .name_len = static_cast<uint8_t>(name.size()),
            .file_type = file_type,
        };
        std::memcpy(block_buf, &new_ent, sizeof(new_ent));
        std::memcpy(block_buf + sizeof(new_ent), name.data(), name.size());

        const int32_t wrc = write_block(new_pblk, block_buf);
        if (wrc != kOk) {
            return wrc;
        }

        dir_disk.i_size += m_block_size;
        return write_disk_inode(dir_ino, dir_disk);
    }

    [[nodiscard]] int32_t dir_remove_entry(uint32_t dir_ino, const Ext2DiskInode& dir_disk,
                                           StringView name) noexcept {
        const uint32_t total_blocks =
            static_cast<uint32_t>((dir_disk.i_size + m_block_size - 1U) / m_block_size);

        uint8_t block_buf[kExt2MaxBlockSize]{};
        Ext2DiskInode mutable_copy = dir_disk;
        bool unused_mod = false;

        for (uint32_t lblk = 0U; lblk < total_blocks; ++lblk) {
            uint32_t pblk = 0U;
            const int32_t brc =
                get_or_alloc_block(dir_ino, mutable_copy, lblk, false, pblk, unused_mod);
            if (brc != kOk) {
                return brc;
            }
            if (pblk == 0U) {
                continue;
            }
            const int32_t rrc = read_block(pblk, block_buf);
            if (rrc != kOk) {
                return rrc;
            }

            size_t off = 0U;
            size_t prev_off = kExt2MaxBlockSize;
            while (off + sizeof(Ext2DirEntry2) <= m_block_size) {
                Ext2DirEntry2 ent{};
                std::memcpy(&ent, block_buf + off, sizeof(ent));
                if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > m_block_size) {
                    break;
                }
                if (ent.inode != 0U && ent.name_len == name.size() &&
                    off + sizeof(Ext2DirEntry2) + ent.name_len <= m_block_size) {
                    const auto* const ename =
                        reinterpret_cast<const char*>(block_buf + off + sizeof(ent));
                    if (StringView(ename, ent.name_len) == name) {
                        if (prev_off != kExt2MaxBlockSize) {
                            Ext2DirEntry2 prev_ent{};
                            std::memcpy(&prev_ent, block_buf + prev_off, sizeof(prev_ent));
                            prev_ent.rec_len =
                                static_cast<uint16_t>(prev_ent.rec_len + ent.rec_len);
                            std::memcpy(block_buf + prev_off, &prev_ent, sizeof(prev_ent));
                        } else {
                            ent.inode = 0U;
                            std::memcpy(block_buf + off, &ent, sizeof(ent));
                        }
                        return write_block(pblk, block_buf);
                    }
                }
                prev_off = off;
                off += ent.rec_len;
            }
        }

        return kEnoent;
    }

    [[nodiscard]] int32_t dir_update_dotdot(const Ext2DiskInode& dir_disk,
                                            uint32_t new_parent_ino) const noexcept {
        if (dir_disk.i_block[0] == 0U) {
            return kEio;
        }
        uint8_t block_buf[kExt2MaxBlockSize]{};
        const int32_t rrc = read_block(dir_disk.i_block[0], block_buf);
        if (rrc != kOk) {
            return rrc;
        }

        size_t off = 0U;
        while (off + sizeof(Ext2DirEntry2) <= m_block_size) {
            Ext2DirEntry2 ent{};
            std::memcpy(&ent, block_buf + off, sizeof(ent));
            if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > m_block_size) {
                break;
            }
            if (ent.inode != 0U && ent.name_len == 2U &&
                off + sizeof(Ext2DirEntry2) + 2U <= m_block_size) {
                const auto* const ename =
                    reinterpret_cast<const char*>(block_buf + off + sizeof(ent));
                if (ename[0] == '.' && ename[1] == '.') {
                    ent.inode = new_parent_ino;
                    std::memcpy(block_buf + off, &ent, sizeof(ent));
                    return write_block(dir_disk.i_block[0], block_buf);
                }
            }
            off += ent.rec_len;
        }
        return kEnoent;
    }

  private:
    [[nodiscard]] uint32_t sectors_per_block() const noexcept { return m_block_size / 512U; }

    [[nodiscard]] int32_t write_superblock() noexcept {
        if (!m_active || m_dev == nullptr) {
            return kEio;
        }
        const size_t sec_size = m_dev->sector_size();
        if (sec_size == 0U || (sizeof(Ext2Superblock) % sec_size) != 0U ||
            (kExt2SuperblockOffset % sec_size) != 0U) {
            return kEio;
        }
        const uint64_t lba = kExt2SuperblockOffset / sec_size;
        const size_t count = sizeof(Ext2Superblock) / sec_size;
        if (m_dev->write_sectors(lba, count, &m_sb) != 0) {
            return kEio;
        }
        m_sb_dirty = false;
        return kOk;
    }

    [[nodiscard]] int32_t write_bgdt() noexcept {
        if (!m_active || m_dev == nullptr) {
            return kEio;
        }
        const size_t total_bytes = static_cast<size_t>(m_group_count) * sizeof(Ext2BlockGroupDesc);
        const uint32_t bgdt_blocks =
            static_cast<uint32_t>((total_bytes + m_block_size - 1U) / m_block_size);

        uint8_t block_buf[kExt2MaxBlockSize]{};
        const auto* const raw_bgdt = reinterpret_cast<const uint8_t*>(m_bgdt);

        for (uint32_t blk_idx = 0U; blk_idx < bgdt_blocks; ++blk_idx) {
            const size_t offset = static_cast<size_t>(blk_idx) * m_block_size;
            const size_t rem = total_bytes - offset;
            const size_t chunk = (rem < m_block_size) ? rem : m_block_size;

            if (chunk < m_block_size) {
                const int32_t rrc = read_block(m_bgdt_start_block + blk_idx, block_buf);
                if (rrc != kOk) {
                    return rrc;
                }
            }
            std::memcpy(block_buf, raw_bgdt + offset, chunk);
            const int32_t wrc = write_block(m_bgdt_start_block + blk_idx, block_buf);
            if (wrc != kOk) {
                return wrc;
            }
        }

        m_bgdt_dirty = false;
        return kOk;
    }

    [[nodiscard]] int32_t alloc_block(uint32_t preferred_group, uint32_t& out_block) noexcept {
        out_block = 0U;
        if (!m_active || m_sb.s_free_blocks_count == 0U) {
            return kEnospc;
        }

        uint8_t bitmap[kExt2MaxBlockSize]{};
        const uint32_t start_group = (preferred_group < m_group_count) ? preferred_group : 0U;

        for (uint32_t offset = 0U; offset < m_group_count; ++offset) {
            const uint32_t grp = (start_group + offset) % m_group_count;
            if (m_bgdt[grp].bg_free_blocks_count == 0U) {
                continue;
            }

            const int32_t rrc = read_block(m_bgdt[grp].bg_block_bitmap, bitmap);
            if (rrc != kOk) {
                return rrc;
            }

            const uint32_t group_first_block =
                m_sb.s_first_data_block + (grp * m_sb.s_blocks_per_group);
            uint32_t blocks_in_group = m_sb.s_blocks_per_group;
            if (group_first_block + blocks_in_group > m_sb.s_blocks_count) {
                blocks_in_group = m_sb.s_blocks_count - group_first_block;
            }

            for (uint32_t bit = 0U; bit < blocks_in_group; ++bit) {
                const uint32_t byte_idx = bit / 8U;
                const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8U));
                if ((bitmap[byte_idx] & mask) == 0U) {
                    bitmap[byte_idx] |= mask;
                    const int32_t wrc = write_block(m_bgdt[grp].bg_block_bitmap, bitmap);
                    if (wrc != kOk) {
                        return wrc;
                    }

                    --m_bgdt[grp].bg_free_blocks_count;
                    --m_sb.s_free_blocks_count;
                    m_bgdt_dirty = true;
                    m_sb_dirty = true;

                    const uint32_t allocated = group_first_block + bit;

                    uint8_t zero_buf[kExt2MaxBlockSize]{};
                    const int32_t zrc = write_block(allocated, zero_buf);
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

    [[nodiscard]] int32_t free_block(uint32_t block_no) noexcept {
        if (!m_active || block_no < m_sb.s_first_data_block || block_no >= m_sb.s_blocks_count) {
            return kEinval;
        }

        const uint32_t rel = block_no - m_sb.s_first_data_block;
        const uint32_t grp = rel / m_sb.s_blocks_per_group;
        const uint32_t bit = rel % m_sb.s_blocks_per_group;
        if (grp >= m_group_count) {
            return kEinval;
        }

        uint8_t bitmap[kExt2MaxBlockSize]{};
        const int32_t rrc = read_block(m_bgdt[grp].bg_block_bitmap, bitmap);
        if (rrc != kOk) {
            return rrc;
        }

        const uint32_t byte_idx = bit / 8U;
        const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8U));
        if ((bitmap[byte_idx] & mask) == 0U) {
            return kOk;
        }

        bitmap[byte_idx] = static_cast<uint8_t>(bitmap[byte_idx] & ~static_cast<uint32_t>(mask));
        const int32_t wrc = write_block(m_bgdt[grp].bg_block_bitmap, bitmap);
        if (wrc != kOk) {
            return wrc;
        }

        ++m_bgdt[grp].bg_free_blocks_count;
        ++m_sb.s_free_blocks_count;
        m_bgdt_dirty = true;
        m_sb_dirty = true;
        return kOk;
    }

    Kernel::Drivers::Block::BlockDevice* m_dev{nullptr};
    Ext2Superblock m_sb{};
    Ext2BlockGroupDesc m_bgdt[kMaxBlockGroups]{};
    uint32_t m_block_size{1024U};
    uint32_t m_inode_size{128U};
    uint32_t m_group_count{0U};
    uint32_t m_ptrs_per_block{256U};
    uint32_t m_bgdt_start_block{2U};
    bool m_sb_dirty{false};
    bool m_bgdt_dirty{false};
    bool m_active{false};
};

Ext2Filesystem g_ext2_fs[kMaxExt2Mounts]{};

[[nodiscard]] Ext2Filesystem* alloc_ext2_fs() noexcept {
    for (auto& slot : g_ext2_fs) {
        if (!slot.is_active()) {
            slot.reset();
            return &slot;
        }
    }
    return nullptr;
}

void free_ext2_fs(Ext2Filesystem* fs) noexcept {
    if (fs != nullptr) {
        fs->reset();
    }
}

[[nodiscard]] Ext2Filesystem* get_ext2_fs(const Superblock* sb) noexcept {
    if (sb == nullptr || sb->get_fs_private() == nullptr) {
        return nullptr;
    }
    return static_cast<Ext2Filesystem*>(sb->get_fs_private());
}

int32_t ext2_sync_fs(Superblock* sb) noexcept;
void ext2_destroy_inode(Inode* inode) noexcept;
int32_t ext2_lookup(Inode* dir, Dentry* target) noexcept;
int32_t ext2_create(Inode* dir, Dentry* target, uint32_t mode) noexcept;
int32_t ext2_mkdir(Inode* dir, Dentry* target, uint32_t mode) noexcept;
int32_t ext2_unlink(Inode* dir, Dentry* target) noexcept;
int32_t ext2_rmdir(Inode* dir, Dentry* target) noexcept;
int32_t ext2_rename(Inode* old_dir, Dentry* old_dentry, Inode* new_dir,
                    Dentry* new_dentry) noexcept;
int32_t ext2_truncate(Inode* inode, uint64_t new_size) noexcept;
int64_t ext2_read(File* file, void* buf, size_t count, uint64_t* offset) noexcept;
int64_t ext2_write(File* file, const void* buf, size_t count, uint64_t* offset) noexcept;
int32_t ext2_fsync(File* file, int32_t datasync) noexcept;
int32_t ext2_readdir(File* file, DirEntryInfo* out_entry) noexcept;

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

[[nodiscard]] RefPtr<Inode> load_vfs_inode(Superblock* sb, uint32_t ino) noexcept {
    auto* const fs = get_ext2_fs(sb);
    if (fs == nullptr) {
        return nullptr;
    }
    Ext2DiskInode disk{};
    if (fs->read_disk_inode(ino, disk) != kOk) {
        return nullptr;
    }

    auto inode = RefPtr<Inode>::adopt(vfs_alloc_inode(sb));
    if (!inode) {
        return nullptr;
    }
    inode->set_ino(ino);
    inode->set_mode(disk.i_mode);
    inode->set_uid(disk.i_uid);
    inode->set_gid(disk.i_gid);
    inode->set_size(disk.i_size);
    inode->set_nlink(disk.i_links_count);
    inode->set_i_ops(&kExt2InodeOps);
    inode->set_f_ops(&kExt2FileOps);
    return inode;
}

int32_t ext2_sync_fs(Superblock* sb) noexcept {
    auto* const fs = get_ext2_fs(sb);
    if (fs == nullptr) {
        return kEinval;
    }
    return fs->sync_metadata();
}

void ext2_destroy_inode(Inode* inode) noexcept {
    if (inode == nullptr) {
        return;
    }
    auto* const fs = get_ext2_fs(inode->get_sb());
    if (fs != nullptr && inode->get_nlink() == 0U && inode->get_ino() != 0U) {
        const auto ino = static_cast<uint32_t>(inode->get_ino());
        Ext2DiskInode disk{};
        if (fs->read_disk_inode(ino, disk) == kOk) {
            fs->free_all_inode_blocks(disk);
            disk.i_size = 0U;
            disk.i_links_count = 0U;
            (void)fs->write_disk_inode(ino, disk);
        }
        (void)fs->free_inode(ino, inode->is_dir());
    }
    inode->reset();
}

int32_t ext2_lookup(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr) {
        return kEinval;
    }
    auto* const fs = get_ext2_fs(dir->get_sb());
    if (fs == nullptr) {
        return kEinval;
    }

    const auto dir_ino = static_cast<uint32_t>(dir->get_ino());
    Ext2DiskInode dir_disk{};
    const int32_t rrc = fs->read_disk_inode(dir_ino, dir_disk);
    if (rrc != kOk) {
        return rrc;
    }

    uint32_t child_ino = 0U;
    const int32_t frc = fs->dir_find_entry(dir_ino, dir_disk, target->name_view(), child_ino);
    if (frc != kOk) {
        return frc;
    }

    auto child_vfs = load_vfs_inode(dir->get_sb(), child_ino);
    if (!child_vfs) {
        return kEnomem;
    }
    target->bind_inode(child_vfs.detach());
    return kOk;
}

int32_t ext2_create(Inode* dir, Dentry* target, uint32_t mode) noexcept {
    if (dir == nullptr || target == nullptr) {
        return kEinval;
    }
    auto* const fs = get_ext2_fs(dir->get_sb());
    if (fs == nullptr) {
        return kEinval;
    }
    const auto dir_ino = static_cast<uint32_t>(dir->get_ino());
    const uint32_t pref_grp = fs->preferred_group_for_ino(dir_ino);

    uint32_t new_ino = 0U;
    int32_t rc = fs->alloc_inode(pref_grp, false, new_ino);
    if (rc != kOk) {
        return rc;
    }

    Ext2DiskInode new_disk{};
    new_disk.i_mode = static_cast<uint16_t>(kModeReg | (mode & 07777U));
    new_disk.i_links_count = 1U;
    rc = fs->write_disk_inode(new_ino, new_disk);
    if (rc != kOk) {
        (void)fs->free_inode(new_ino, false);
        return rc;
    }

    Ext2DiskInode dir_disk{};
    rc = fs->read_disk_inode(dir_ino, dir_disk);
    if (rc != kOk) {
        (void)fs->free_inode(new_ino, false);
        return rc;
    }

    rc = fs->dir_add_entry(dir_ino, dir_disk, target->name_view(), new_ino, kExt2FtRegFile);
    if (rc != kOk) {
        (void)fs->free_inode(new_ino, false);
        return rc;
    }

    dir->set_size(dir_disk.i_size);
    auto vfs_ino = load_vfs_inode(dir->get_sb(), new_ino);
    if (!vfs_ino) {
        return kEnomem;
    }
    target->bind_inode(vfs_ino.detach());
    return kOk;
}

int32_t ext2_mkdir(Inode* dir, Dentry* target, uint32_t mode) noexcept {
    if (dir == nullptr || target == nullptr) {
        return kEinval;
    }
    auto* const fs = get_ext2_fs(dir->get_sb());
    if (fs == nullptr) {
        return kEinval;
    }
    const auto parent_ino = static_cast<uint32_t>(dir->get_ino());
    const uint32_t pref_grp = fs->preferred_group_for_ino(parent_ino);

    uint32_t new_ino = 0U;
    int32_t rc = fs->alloc_inode(pref_grp, true, new_ino);
    if (rc != kOk) {
        return rc;
    }

    uint32_t dir_blk = 0U;
    bool unused_mod = false;
    Ext2DiskInode dummy_inode{};
    rc = fs->get_or_alloc_block(new_ino, dummy_inode, 0U, true, dir_blk, unused_mod);
    if (rc != kOk) {
        (void)fs->free_inode(new_ino, true);
        return rc;
    }

    const uint32_t blk_sz = fs->block_size();
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
        .rec_len = static_cast<uint16_t>(blk_sz - dot_len),
        .name_len = 2U,
        .file_type = kExt2FtDir,
    };
    std::memcpy(block_buf + dot_len, &dotdot_ent, sizeof(dotdot_ent));
    block_buf[dot_len + sizeof(dotdot_ent)] = '.';
    block_buf[dot_len + sizeof(dotdot_ent) + 1U] = '.';

    rc = fs->write_block(dir_blk, block_buf);
    if (rc != kOk) {
        fs->free_all_inode_blocks(dummy_inode);
        (void)fs->free_inode(new_ino, true);
        return rc;
    }

    Ext2DiskInode new_disk{};
    new_disk.i_mode = static_cast<uint16_t>(kModeDir | (mode & 07777U));
    new_disk.i_size = blk_sz;
    new_disk.i_links_count = 2U;
    new_disk.i_blocks = blk_sz / 512U;
    new_disk.i_block[0] = dir_blk;

    rc = fs->write_disk_inode(new_ino, new_disk);
    if (rc != kOk) {
        fs->free_all_inode_blocks(new_disk);
        (void)fs->free_inode(new_ino, true);
        return rc;
    }

    Ext2DiskInode parent_disk{};
    rc = fs->read_disk_inode(parent_ino, parent_disk);
    if (rc != kOk) {
        fs->free_all_inode_blocks(new_disk);
        (void)fs->free_inode(new_ino, true);
        return rc;
    }

    rc = fs->dir_add_entry(parent_ino, parent_disk, target->name_view(), new_ino, kExt2FtDir);
    if (rc != kOk) {
        fs->free_all_inode_blocks(new_disk);
        (void)fs->free_inode(new_ino, true);
        return rc;
    }

    ++parent_disk.i_links_count;
    rc = fs->write_disk_inode(parent_ino, parent_disk);
    if (rc != kOk) {
        return rc;
    }

    dir->set_nlink(parent_disk.i_links_count);
    dir->set_size(parent_disk.i_size);

    auto vfs_ino = load_vfs_inode(dir->get_sb(), new_ino);
    if (!vfs_ino) {
        return kEnomem;
    }
    target->bind_inode(vfs_ino.detach());
    return kOk;
}

int32_t ext2_unlink(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || target->get_inode() == nullptr) {
        return kEinval;
    }
    auto* const fs = get_ext2_fs(dir->get_sb());
    if (fs == nullptr) {
        return kEinval;
    }
    const auto dir_ino = static_cast<uint32_t>(dir->get_ino());

    Ext2DiskInode dir_disk{};
    int32_t rc = fs->read_disk_inode(dir_ino, dir_disk);
    if (rc != kOk) {
        return rc;
    }

    rc = fs->dir_remove_entry(dir_ino, dir_disk, target->name_view());
    if (rc != kOk) {
        return rc;
    }

    Inode* const child_vfs = target->get_inode();
    const auto child_ino = static_cast<uint32_t>(child_vfs->get_ino());
    Ext2DiskInode child_disk{};
    rc = fs->read_disk_inode(child_ino, child_disk);
    if (rc != kOk) {
        return rc;
    }

    if (child_disk.i_links_count > 0U) {
        --child_disk.i_links_count;
    }
    child_vfs->set_nlink(child_disk.i_links_count);
    return fs->write_disk_inode(child_ino, child_disk);
}

int32_t ext2_rmdir(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || target->get_inode() == nullptr) {
        return kEinval;
    }
    auto* const fs = get_ext2_fs(dir->get_sb());
    if (fs == nullptr) {
        return kEinval;
    }
    const auto parent_ino = static_cast<uint32_t>(dir->get_ino());
    Inode* const child_vfs = target->get_inode();
    const auto child_ino = static_cast<uint32_t>(child_vfs->get_ino());

    Ext2DiskInode child_disk{};
    int32_t rc = fs->read_disk_inode(child_ino, child_disk);
    if (rc != kOk) {
        return rc;
    }

    bool empty = false;
    rc = fs->dir_is_empty(child_ino, child_disk, empty);
    if (rc != kOk) {
        return rc;
    }
    if (!empty) {
        return kEnotempty;
    }

    Ext2DiskInode parent_disk{};
    rc = fs->read_disk_inode(parent_ino, parent_disk);
    if (rc != kOk) {
        return rc;
    }

    rc = fs->dir_remove_entry(parent_ino, parent_disk, target->name_view());
    if (rc != kOk) {
        return rc;
    }

    if (parent_disk.i_links_count > 0U) {
        --parent_disk.i_links_count;
    }
    rc = fs->write_disk_inode(parent_ino, parent_disk);
    if (rc != kOk) {
        return rc;
    }
    dir->set_nlink(parent_disk.i_links_count);

    child_disk.i_links_count = 0U;
    child_vfs->set_nlink(0U);
    return fs->write_disk_inode(child_ino, child_disk);
}

int32_t ext2_rename(Inode* old_dir, Dentry* old_dentry, Inode* new_dir,
                    Dentry* new_dentry) noexcept {
    if (old_dir == nullptr || old_dentry == nullptr || old_dentry->get_inode() == nullptr ||
        new_dir == nullptr || new_dentry == nullptr) {
        return kEinval;
    }
    auto* const fs = get_ext2_fs(old_dir->get_sb());
    if (fs == nullptr) {
        return kEinval;
    }
    const auto old_dir_ino = static_cast<uint32_t>(old_dir->get_ino());
    const auto new_dir_ino = static_cast<uint32_t>(new_dir->get_ino());
    Inode* const src_vfs = old_dentry->get_inode();
    const auto src_ino = static_cast<uint32_t>(src_vfs->get_ino());

    Ext2DiskInode new_dir_disk{};
    int32_t rc = fs->read_disk_inode(new_dir_ino, new_dir_disk);
    if (rc != kOk) {
        return rc;
    }

    if (Inode* const dst_vfs = new_dentry->get_inode(); dst_vfs != nullptr) {
        const auto dst_ino = static_cast<uint32_t>(dst_vfs->get_ino());
        Ext2DiskInode dst_disk{};
        rc = fs->read_disk_inode(dst_ino, dst_disk);
        if (rc != kOk) {
            return rc;
        }
        if (dst_vfs->is_dir()) {
            bool empty = false;
            rc = fs->dir_is_empty(dst_ino, dst_disk, empty);
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

        rc = fs->dir_remove_entry(new_dir_ino, new_dir_disk, new_dentry->name_view());
        if (rc != kOk) {
            return rc;
        }
        dst_vfs->set_nlink(dst_disk.i_links_count);
        rc = fs->write_disk_inode(dst_ino, dst_disk);
        if (rc != kOk) {
            return rc;
        }
    }

    const uint8_t ft = mode_to_ext2_ft(src_vfs->get_mode());
    rc = fs->dir_add_entry(new_dir_ino, new_dir_disk, new_dentry->name_view(), src_ino, ft);
    if (rc != kOk) {
        return rc;
    }

    Ext2DiskInode old_dir_disk{};
    rc = fs->read_disk_inode(old_dir_ino, old_dir_disk);
    if (rc != kOk) {
        return rc;
    }

    rc = fs->dir_remove_entry(old_dir_ino, old_dir_disk, old_dentry->name_view());
    if (rc != kOk) {
        return rc;
    }

    if (src_vfs->is_dir() && old_dir_ino != new_dir_ino) {
        Ext2DiskInode src_disk{};
        rc = fs->read_disk_inode(src_ino, src_disk);
        if (rc != kOk) {
            return rc;
        }
        rc = fs->dir_update_dotdot(src_disk, new_dir_ino);
        if (rc != kOk) {
            return rc;
        }
        if (old_dir_disk.i_links_count > 0U) {
            --old_dir_disk.i_links_count;
        }
        ++new_dir_disk.i_links_count;
        rc = fs->write_disk_inode(old_dir_ino, old_dir_disk);
        if (rc != kOk) {
            return rc;
        }
        old_dir->set_nlink(old_dir_disk.i_links_count);
    }

    rc = fs->write_disk_inode(new_dir_ino, new_dir_disk);
    if (rc != kOk) {
        return rc;
    }
    new_dir->set_nlink(new_dir_disk.i_links_count);
    new_dir->set_size(new_dir_disk.i_size);
    return kOk;
}

int32_t ext2_truncate(Inode* inode, uint64_t new_size) noexcept {
    if (inode == nullptr) {
        return kEinval;
    }
    auto* const fs = get_ext2_fs(inode->get_sb());
    if (fs == nullptr) {
        return kEinval;
    }
    if (inode->is_dir()) {
        return kEisdir;
    }
    if (new_size > UINT32_MAX) {
        return kEnospc;
    }

    const auto ino = static_cast<uint32_t>(inode->get_ino());
    Ext2DiskInode disk{};
    int32_t rc = fs->read_disk_inode(ino, disk);
    if (rc != kOk) {
        return rc;
    }

    const uint32_t blk_sz = fs->block_size();
    if (new_size == 0U) {
        fs->free_all_inode_blocks(disk);
    } else if (new_size < disk.i_size) {
        const uint32_t keep_blocks = static_cast<uint32_t>((new_size + blk_sz - 1U) / blk_sz);
        const uint32_t old_blocks = static_cast<uint32_t>((disk.i_size + blk_sz - 1U) / blk_sz);

        const size_t tail_off = static_cast<size_t>(new_size % blk_sz);
        if (tail_off != 0U && keep_blocks > 0U) {
            uint32_t last_pblk = 0U;
            bool unused_mod = false;
            if (fs->get_or_alloc_block(ino, disk, keep_blocks - 1U, false, last_pblk, unused_mod) ==
                    kOk &&
                last_pblk != 0U) {
                uint8_t block_buf[kExt2MaxBlockSize]{};
                if (fs->read_block(last_pblk, block_buf) == kOk) {
                    std::memset(block_buf + tail_off, 0, blk_sz - tail_off);
                    (void)fs->write_block(last_pblk, block_buf);
                }
            }
        }

        for (uint32_t blk = keep_blocks; blk < old_blocks && blk < kDirectBlocks; ++blk) {
            if (disk.i_block[blk] != 0U) {
                fs->free_all_inode_blocks(disk);
                break;
            }
        }
    }

    disk.i_size = static_cast<uint32_t>(new_size);
    rc = fs->write_disk_inode(ino, disk);
    if (rc != kOk) {
        return rc;
    }

    inode->set_size(new_size);
    return kOk;
}

int64_t ext2_read(File* file, void* buf, size_t count, uint64_t* offset) noexcept {
    if (file == nullptr || file->get_inode() == nullptr || offset == nullptr) {
        return kEinval;
    }
    Inode* const inode = file->get_inode();
    auto* const fs = get_ext2_fs(inode->get_sb());
    if (fs == nullptr) {
        return kEinval;
    }
    if (inode->is_dir()) {
        return kEisdir;
    }
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kEinval;
    }

    const auto ino = static_cast<uint32_t>(inode->get_ino());
    Ext2DiskInode disk{};
    const int32_t rrc = fs->read_disk_inode(ino, disk);
    if (rrc != kOk) {
        return rrc;
    }

    if (*offset >= disk.i_size) {
        return 0;
    }

    const uint32_t blk_sz = fs->block_size();
    const uint64_t avail = disk.i_size - *offset;
    const size_t to_read =
        (static_cast<uint64_t>(count) < avail) ? count : static_cast<size_t>(avail);

    auto* const dst = static_cast<uint8_t*>(buf);
    uint8_t block_buf[kExt2MaxBlockSize]{};
    size_t total_read = 0U;
    bool unused_mod = false;

    while (total_read < to_read) {
        const uint32_t lblk = static_cast<uint32_t>(*offset / blk_sz);
        const size_t boff = static_cast<size_t>(*offset % blk_sz);
        const size_t brem = blk_sz - boff;
        const size_t rem = to_read - total_read;
        const size_t chunk = (rem < brem) ? rem : brem;

        uint32_t pblk = 0U;
        const int32_t brc = fs->get_or_alloc_block(ino, disk, lblk, false, pblk, unused_mod);
        if (brc != kOk) {
            return (total_read > 0U) ? static_cast<int64_t>(total_read) : brc;
        }

        if (pblk == 0U) {
            std::memset(dst + total_read, 0, chunk);
        } else {
            const int32_t rc = fs->read_block(pblk, block_buf);
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
    if (file == nullptr || file->get_inode() == nullptr || offset == nullptr) {
        return kEinval;
    }
    Inode* const inode = file->get_inode();
    auto* const fs = get_ext2_fs(inode->get_sb());
    if (fs == nullptr) {
        return kEinval;
    }
    if (inode->is_dir()) {
        return kEisdir;
    }
    if (count == 0U) {
        return 0;
    }
    if (buf == nullptr) {
        return kEinval;
    }

    const auto ino = static_cast<uint32_t>(inode->get_ino());
    Ext2DiskInode disk{};
    const int32_t rrc = fs->read_disk_inode(ino, disk);
    if (rrc != kOk) {
        return rrc;
    }

    const uint32_t blk_sz = fs->block_size();
    const auto* const src = static_cast<const uint8_t*>(buf);
    uint8_t block_buf[kExt2MaxBlockSize]{};
    size_t total_written = 0U;
    bool inode_mod = false;

    while (total_written < count) {
        const uint32_t lblk = static_cast<uint32_t>(*offset / blk_sz);
        const size_t boff = static_cast<size_t>(*offset % blk_sz);
        const size_t brem = blk_sz - boff;
        const size_t rem = count - total_written;
        const size_t chunk = (rem < brem) ? rem : brem;

        uint32_t pblk = 0U;
        const int32_t brc = fs->get_or_alloc_block(ino, disk, lblk, true, pblk, inode_mod);
        if (brc != kOk) {
            if (inode_mod) {
                (void)fs->write_disk_inode(ino, disk);
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : brc;
        }

        if (chunk < blk_sz) {
            const int32_t rc = fs->read_block(pblk, block_buf);
            if (rc != kOk) {
                if (inode_mod) {
                    (void)fs->write_disk_inode(ino, disk);
                }
                return (total_written > 0U) ? static_cast<int64_t>(total_written) : rc;
            }
        }

        std::memcpy(block_buf + boff, src + total_written, chunk);
        const int32_t wrc = fs->write_block(pblk, block_buf);
        if (wrc != kOk) {
            if (inode_mod) {
                (void)fs->write_disk_inode(ino, disk);
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : wrc;
        }

        *offset += chunk;
        total_written += chunk;
    }

    if (*offset > disk.i_size) {
        disk.i_size = static_cast<uint32_t>(*offset);
        inode->set_size(*offset);
        inode_mod = true;
    }

    if (inode_mod) {
        const int32_t irc = fs->write_disk_inode(ino, disk);
        if (irc != kOk) {
            return irc;
        }
    }

    return static_cast<int64_t>(total_written);
}

int32_t ext2_fsync(File* file, int32_t /*datasync*/) noexcept {
    if (file == nullptr || file->get_inode() == nullptr) {
        return kEbadf;
    }
    auto* const fs = get_ext2_fs(file->get_inode()->get_sb());
    if (fs == nullptr) {
        return kEbadf;
    }
    return fs->sync_metadata();
}

int32_t ext2_readdir(File* file, DirEntryInfo* out_entry) noexcept {
    if (file == nullptr || file->get_inode() == nullptr || out_entry == nullptr) {
        return kEinval;
    }
    auto* const fs = get_ext2_fs(file->get_inode()->get_sb());
    if (fs == nullptr) {
        return kEinval;
    }

    const auto dir_ino = static_cast<uint32_t>(file->get_inode()->get_ino());
    Ext2DiskInode dir_disk{};
    const int32_t rrc = fs->read_disk_inode(dir_ino, dir_disk);
    if (rrc != kOk) {
        return rrc;
    }

    const uint32_t blk_sz = fs->block_size();
    const uint32_t total_blocks = static_cast<uint32_t>((dir_disk.i_size + blk_sz - 1U) / blk_sz);

    uint8_t block_buf[kExt2MaxBlockSize]{};
    const uint64_t target_idx = file->get_offset();
    uint64_t logical_idx = 0U;
    bool unused_mod = false;

    for (uint32_t lblk = 0U; lblk < total_blocks; ++lblk) {
        uint32_t pblk = 0U;
        const int32_t brc =
            fs->get_or_alloc_block(dir_ino, dir_disk, lblk, false, pblk, unused_mod);
        if (brc != kOk) {
            return brc;
        }
        if (pblk == 0U) {
            continue;
        }
        const int32_t rc = fs->read_block(pblk, block_buf);
        if (rc != kOk) {
            return rc;
        }

        size_t off = 0U;
        while (off + sizeof(Ext2DirEntry2) <= blk_sz) {
            Ext2DirEntry2 ent{};
            std::memcpy(&ent, block_buf + off, sizeof(ent));
            if (ent.rec_len < sizeof(Ext2DirEntry2) || off + ent.rec_len > blk_sz) {
                break;
            }
            if (ent.inode != 0U && ent.name_len > 0U &&
                off + sizeof(Ext2DirEntry2) + ent.name_len <= blk_sz) {
                const auto* const ename =
                    reinterpret_cast<const char*>(block_buf + off + sizeof(ent));
                const StringView sv(ename, ent.name_len);
                if (sv != "." && sv != "..") {
                    if (logical_idx == target_idx) {
                        out_entry->ino = ent.inode;
                        out_entry->type = ext2_ft_to_vfs_type(ent.file_type);
                        const size_t copy_len =
                            (ent.name_len < kMaxNameLen) ? ent.name_len : kMaxNameLen;
                        for (size_t i = 0U; i < copy_len; ++i) {
                            out_entry->name[i] = ename[i];
                        }
                        out_entry->name[copy_len] = '\0';
                        file->set_offset(target_idx + 1U);
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

void release_dentry_subtree(Dentry* node) noexcept {
    if (node == nullptr) {
        return;
    }
    Dentry* child = node->get_first_child();
    while (child != nullptr) {
        Dentry* const next = child->get_next_sibling();
        child->detach();
        release_dentry_subtree(child);
        child = next;
    }
    node->release();
}

} // namespace

void ext2_reset() noexcept {
    for (auto& slot : g_ext2_fs) {
        slot.reset();
    }
}

int32_t ext2_create_sb(Kernel::Drivers::Block::BlockDevice* dev, Superblock** out_sb) noexcept {
    if (dev == nullptr || out_sb == nullptr) {
        return kEinval;
    }
    *out_sb = nullptr;

    Ext2Filesystem* const fs = alloc_ext2_fs();
    if (fs == nullptr) {
        return kEnomem;
    }

    const int32_t mrc = fs->mount_device(dev);
    if (mrc != kOk) {
        free_ext2_fs(fs);
        return mrc;
    }

    Superblock* const superblock = vfs_alloc_superblock();
    if (superblock == nullptr) {
        free_ext2_fs(fs);
        return kEnomem;
    }
    superblock->set_fs_type("ext2");
    superblock->set_block_size(fs->block_size());
    superblock->set_flags(0U);
    superblock->set_ops(&kExt2SbOps);
    superblock->set_fs_private(fs);

    auto root_inode = load_vfs_inode(superblock, kExt2RootIno);
    if (!root_inode || !root_inode->is_dir()) {
        vfs_free_superblock(superblock);
        free_ext2_fs(fs);
        return kEinval;
    }

    auto root_dentry = RefPtr<Dentry>::adopt(vfs_alloc_dentry(superblock, nullptr, "ext2"));
    if (!root_dentry) {
        vfs_free_superblock(superblock);
        free_ext2_fs(fs);
        return kEnomem;
    }
    root_dentry->bind_inode(root_inode.detach());
    superblock->set_root(root_dentry.detach());

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
        release_dentry_subtree(sb->get_root());
        free_ext2_fs(get_ext2_fs(sb));
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
        release_dentry_subtree(sb->get_root());
        free_ext2_fs(get_ext2_fs(sb));
        vfs_free_superblock(sb);
    }
    return kOk;
}

} // namespace Kernel::FS