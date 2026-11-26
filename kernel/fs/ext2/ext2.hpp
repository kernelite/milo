#pragma once

#include "drivers/block/block_device.hpp"
#include "fs/vfs.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::FS {

inline constexpr uint16_t kExt2SuperMagic = 0xEF53U;
inline constexpr uint32_t kExt2RootIno = 2U;
inline constexpr size_t kExt2SuperblockOffset = 1024U;
inline constexpr size_t kExt2MaxBlockSize = 4096U;

// File type constants stored in ext2_dir_entry_2::file_type
inline constexpr uint8_t kExt2FtUnknown = 0U;
inline constexpr uint8_t kExt2FtRegFile = 1U;
inline constexpr uint8_t kExt2FtDir = 2U;
inline constexpr uint8_t kExt2FtChrdev = 3U;
inline constexpr uint8_t kExt2FtBlkdev = 4U;
inline constexpr uint8_t kExt2FtFifo = 5U;
inline constexpr uint8_t kExt2FtSock = 6U;
inline constexpr uint8_t kExt2FtSymlink = 7U;

// On-disk ext2 Superblock (1024 bytes at byte offset 1024)
struct [[gnu::packed]] Ext2Superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // Dynamic revision (rev >= 1) fields
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16];
    char s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t s_reserved[820];
};
static_assert(sizeof(Ext2Superblock) == 1024U, "Ext2Superblock must be 1024 bytes");

// On-disk Block Group Descriptor (32 bytes)
struct [[gnu::packed]] Ext2BlockGroupDesc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t bg_reserved[12];
};
static_assert(sizeof(Ext2BlockGroupDesc) == 32U, "Ext2BlockGroupDesc must be 32 bytes");

// On-disk ext2 Inode (128 bytes base)
struct [[gnu::packed]] Ext2DiskInode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks; // Count of 512-byte sectors
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15]; // 0..11 direct, 12 singly indirect, 13 doubly indirect, 14 triply
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t i_osd2[12];
};
static_assert(sizeof(Ext2DiskInode) == 128U, "Ext2DiskInode must be 128 bytes");

// On-disk variable-length directory entry header (8 bytes + name)
struct [[gnu::packed]] Ext2DirEntry2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
};
static_assert(sizeof(Ext2DirEntry2) == 8U, "Ext2DirEntry2 header must be 8 bytes");

// Resets internal ext2 driver state (for host unit tests).
void ext2_reset() noexcept;

// Creates a VFS Superblock backed by an ext2 filesystem on `dev`.
[[nodiscard]] int32_t ext2_create_sb(Kernel::Drivers::Block::BlockDevice* dev,
                                     Superblock** out_sb) noexcept;

// Mounts an ext2 filesystem from `dev` at `mount_path` (e.g., "/mnt/disk").
[[nodiscard]] int32_t ext2_mount(Kernel::Drivers::Block::BlockDevice* dev, StringView mount_path,
                                 Superblock** out_sb = nullptr) noexcept;

// Flushes, unmounts `mount_path`, and releases ext2 superblock resources.
[[nodiscard]] int32_t ext2_umount(StringView mount_path, Superblock* sb) noexcept;

} // namespace Kernel::FS