#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef HOST_TEST
#undef AT_FDCWD
#undef AT_SYMLINK_NOFOLLOW
#undef AT_EACCESS
#undef AT_REMOVEDIR
#undef AT_EMPTY_PATH
#undef O_ACCMODE
#undef O_RDONLY
#undef O_WRONLY
#undef O_RDWR
#undef O_CREAT
#undef O_EXCL
#undef O_TRUNC
#undef O_APPEND
#undef O_DIRECTORY
#undef O_NONBLOCK
#undef O_CLOEXEC
#undef RENAME_NOREPLACE
#undef UTIME_NOW
#undef UTIME_OMIT
#undef F_DUPFD
#undef F_GETFD
#undef F_SETFD
#undef F_GETFL
#undef F_SETFL
#undef F_DUPFD_CLOEXEC
#undef SEEK_SET
#undef SEEK_CUR
#undef SEEK_END
#undef DT_FIFO
#undef DT_CHR
#undef DT_DIR
#undef DT_BLK
#undef DT_REG
#undef DT_LNK
#undef UIO_MAXIOV
#endif

namespace Kernel::UAPI {

inline constexpr size_t UIO_MAXIOV = 1024U;

// *at() directory descriptor & lookup flags
inline constexpr int32_t AT_FDCWD = -100;
inline constexpr uint64_t AT_SYMLINK_NOFOLLOW = 0x100ULL;
inline constexpr uint64_t AT_EACCESS = 0x200ULL;
inline constexpr uint64_t AT_REMOVEDIR = 0x200ULL;
inline constexpr uint64_t AT_EMPTY_PATH = 0x1000ULL;

// Linux AArch64 openat() flags (O_DIRECTORY is 040000 on AArch64)
inline constexpr uint64_t O_ACCMODE = 0x3ULL;
inline constexpr uint64_t O_RDONLY = 0x0ULL;
inline constexpr uint64_t O_WRONLY = 0x1ULL;
inline constexpr uint64_t O_RDWR = 0x2ULL;
inline constexpr uint64_t O_CREAT = 0x40ULL;
inline constexpr uint64_t O_EXCL = 0x80ULL;
inline constexpr uint64_t O_TRUNC = 0x200ULL;
inline constexpr uint64_t O_APPEND = 0x400ULL;
inline constexpr uint64_t O_NONBLOCK = 0x800ULL;
inline constexpr uint64_t O_DIRECTORY = 0x4000ULL;
inline constexpr uint64_t O_CLOEXEC = 0x80000ULL;

// renameat2() & utimensat() constants
inline constexpr uint64_t RENAME_NOREPLACE = 1ULL;
inline constexpr int64_t UTIME_NOW = 0x3FFFFFFFLL;
inline constexpr int64_t UTIME_OMIT = 0x3FFFFFFELL;

// fcntl() commands & descriptor flags
inline constexpr uint64_t F_DUPFD = 0ULL;
inline constexpr uint64_t F_GETFD = 1ULL;
inline constexpr uint64_t F_SETFD = 2ULL;
inline constexpr uint64_t F_GETFL = 3ULL;
inline constexpr uint64_t F_SETFL = 4ULL;
inline constexpr uint64_t F_DUPFD_CLOEXEC = 1030ULL;
inline constexpr uint64_t FD_CLOEXEC_BIT = 1ULL;

// lseek() whence values
inline constexpr uint64_t SEEK_SET = 0ULL;
inline constexpr uint64_t SEEK_CUR = 1ULL;
inline constexpr uint64_t SEEK_END = 2ULL;

// getdents64() d_type values
inline constexpr uint8_t DT_FIFO = 1U;
inline constexpr uint8_t DT_CHR = 2U;
inline constexpr uint8_t DT_DIR = 4U;
inline constexpr uint8_t DT_BLK = 6U;
inline constexpr uint8_t DT_REG = 8U;
inline constexpr uint8_t DT_LNK = 10U;

} // namespace Kernel::UAPI

extern "C" struct linux_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};
static_assert(std::is_standard_layout_v<linux_iovec>, "linux_iovec must be standard layout");
static_assert(std::is_trivial_v<linux_iovec>, "linux_iovec must be trivial");
static_assert(sizeof(linux_iovec) == 16U, "linux_iovec must match Linux struct iovec ABI");
static_assert(offsetof(linux_iovec, iov_base) == 0U);
static_assert(offsetof(linux_iovec, iov_len) == 8U);

// Linux AArch64 struct stat (128 bytes)
extern "C" struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t pad2;
    int64_t st_blocks;
    int64_t st_atime_sec;
    int64_t st_atime_nsec;
    int64_t st_mtime_sec;
    int64_t st_mtime_nsec;
    int64_t st_ctime_sec;
    int64_t st_ctime_nsec;
    uint32_t unused4;
    uint32_t unused5;
};
static_assert(std::is_standard_layout_v<linux_stat>, "linux_stat must be standard layout");
static_assert(std::is_trivial_v<linux_stat>, "linux_stat must be trivial");
static_assert(sizeof(linux_stat) == 128U, "linux_stat must match Linux AArch64 ABI (128 bytes)");
static_assert(offsetof(linux_stat, st_dev) == 0U);
static_assert(offsetof(linux_stat, st_ino) == 8U);
static_assert(offsetof(linux_stat, st_mode) == 16U);
static_assert(offsetof(linux_stat, st_nlink) == 20U);
static_assert(offsetof(linux_stat, st_uid) == 24U);
static_assert(offsetof(linux_stat, st_gid) == 28U);
static_assert(offsetof(linux_stat, st_rdev) == 32U);
static_assert(offsetof(linux_stat, st_size) == 48U);
static_assert(offsetof(linux_stat, st_blksize) == 56U);
static_assert(offsetof(linux_stat, st_blocks) == 64U);
static_assert(offsetof(linux_stat, st_atime_sec) == 72U);
static_assert(offsetof(linux_stat, st_mtime_sec) == 88U);
static_assert(offsetof(linux_stat, st_ctime_sec) == 104U);

extern "C" struct [[gnu::packed]] linux_dirent64_hdr {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
};
static_assert(std::is_standard_layout_v<linux_dirent64_hdr>,
              "linux_dirent64_hdr must be standard layout");
static_assert(std::is_trivial_v<linux_dirent64_hdr>, "linux_dirent64_hdr must be trivial");
static_assert(sizeof(linux_dirent64_hdr) == 19U, "linux_dirent64_hdr must be 19 bytes");
static_assert(offsetof(linux_dirent64_hdr, d_ino) == 0U);
static_assert(offsetof(linux_dirent64_hdr, d_off) == 8U);
static_assert(offsetof(linux_dirent64_hdr, d_reclen) == 16U);
static_assert(offsetof(linux_dirent64_hdr, d_type) == 18U);
