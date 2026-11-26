#pragma once

#include <cstdint>

namespace Kernel::UAPI {

// Negated POSIX / Linux asm-generic errno constants
inline constexpr int64_t ERR_EPERM = -1;
inline constexpr int64_t ERR_ENOENT = -2;
inline constexpr int64_t ERR_ESRCH = -3;
inline constexpr int64_t ERR_EINTR = -4;
inline constexpr int64_t ERR_EIO = -5;
inline constexpr int64_t ERR_ENXIO = -6;
inline constexpr int64_t ERR_E2BIG = -7;
inline constexpr int64_t ERR_ENOEXEC = -8;
inline constexpr int64_t ERR_EBADF = -9;
inline constexpr int64_t ERR_ECHILD = -10;
inline constexpr int64_t ERR_EAGAIN = -11;
inline constexpr int64_t ERR_ENOMEM = -12;
inline constexpr int64_t ERR_EACCES = -13;
inline constexpr int64_t ERR_EFAULT = -14;
inline constexpr int64_t ERR_EBUSY = -16;
inline constexpr int64_t ERR_EEXIST = -17;
inline constexpr int64_t ERR_EXDEV = -18;
inline constexpr int64_t ERR_ENODEV = -19;
inline constexpr int64_t ERR_ENOTDIR = -20;
inline constexpr int64_t ERR_EISDIR = -21;
inline constexpr int64_t ERR_EINVAL = -22;
inline constexpr int64_t ERR_ENFILE = -23;
inline constexpr int64_t ERR_EMFILE = -24;
inline constexpr int64_t ERR_ENOTTY = -25;
inline constexpr int64_t ERR_EFBIG = -27;
inline constexpr int64_t ERR_ENOSPC = -28;
inline constexpr int64_t ERR_ESPIPE = -29;
inline constexpr int64_t ERR_EROFS = -30;
inline constexpr int64_t ERR_EMLINK = -31;
inline constexpr int64_t ERR_EPIPE = -32;
inline constexpr int64_t ERR_EDOM = -33;
inline constexpr int64_t ERR_ERANGE = -34;
inline constexpr int64_t ERR_EDEADLK = -35;
inline constexpr int64_t ERR_ENAMETOOLONG = -36;
inline constexpr int64_t ERR_ENOLCK = -37;
inline constexpr int64_t ERR_ENOSYS = -38;

} // namespace Kernel::UAPI
