#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef HOST_TEST
#undef TCGETS
#undef TCSETS
#undef TCSETSW
#undef TCSETSF
#undef TIOCGPGRP
#undef TIOCSPGRP
#undef TIOCGWINSZ
#undef INLCR
#undef IGNCR
#undef ICRNL
#undef OPOST
#undef ONLCR
#undef CS8
#undef CREAD
#undef CLOCAL
#undef ISIG
#undef ICANON
#undef ECHO
#undef ECHOE
#undef ECHOK
#undef VINTR
#undef VQUIT
#undef VERASE
#undef VKILL
#undef VEOF
#undef VTIME
#undef VMIN
#undef NCCS
#endif

namespace Kernel::UAPI {

// Terminal ioctl request codes (Linux asm-generic)
inline constexpr uint64_t TCGETS = 0x5401ULL;
inline constexpr uint64_t TCSETS = 0x5402ULL;
inline constexpr uint64_t TCSETSW = 0x5403ULL;
inline constexpr uint64_t TCSETSF = 0x5404ULL;
inline constexpr uint64_t TIOCGPGRP = 0x540FULL;
inline constexpr uint64_t TIOCSPGRP = 0x5410ULL;
inline constexpr uint64_t TIOCGWINSZ = 0x5413ULL;

// termios input, output, control, and local flag bits
inline constexpr uint32_t INLCR = 0x0040U;
inline constexpr uint32_t IGNCR = 0x0080U;
inline constexpr uint32_t ICRNL = 0x0100U;

inline constexpr uint32_t OPOST = 0x0001U;
inline constexpr uint32_t ONLCR = 0x0004U;

inline constexpr uint32_t CS8 = 0x0030U;
inline constexpr uint32_t CREAD = 0x0080U;
inline constexpr uint32_t CLOCAL = 0x0800U;

inline constexpr uint32_t ISIG = 0x0001U;
inline constexpr uint32_t ICANON = 0x0002U;
inline constexpr uint32_t ECHO = 0x0008U;
inline constexpr uint32_t ECHOE = 0x0010U;
inline constexpr uint32_t ECHOK = 0x0020U;

// termios c_cc control character indices & array length
inline constexpr size_t VINTR = 0U;
inline constexpr size_t VQUIT = 1U;
inline constexpr size_t VERASE = 2U;
inline constexpr size_t VKILL = 3U;
inline constexpr size_t VEOF = 4U;
inline constexpr size_t VTIME = 5U;
inline constexpr size_t VMIN = 6U;
inline constexpr size_t NCCS = 19U;

} // namespace Kernel::UAPI

// Linux asm-generic struct ktermios (36 bytes)
extern "C" struct linux_ktermios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
};
static_assert(std::is_standard_layout_v<linux_ktermios>, "linux_ktermios must be standard layout");
static_assert(std::is_trivial_v<linux_ktermios>, "linux_ktermios must be trivial");
static_assert(sizeof(linux_ktermios) == 36U,
              "linux_ktermios must match Linux asm-generic ABI (36 bytes)");
static_assert(offsetof(linux_ktermios, c_iflag) == 0U);
static_assert(offsetof(linux_ktermios, c_oflag) == 4U);
static_assert(offsetof(linux_ktermios, c_cflag) == 8U);
static_assert(offsetof(linux_ktermios, c_lflag) == 12U);
static_assert(offsetof(linux_ktermios, c_line) == 16U);
static_assert(offsetof(linux_ktermios, c_cc) == 17U);

extern "C" struct linux_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};
static_assert(std::is_standard_layout_v<linux_winsize>, "linux_winsize must be standard layout");
static_assert(std::is_trivial_v<linux_winsize>, "linux_winsize must be trivial");
static_assert(sizeof(linux_winsize) == 8U, "linux_winsize must be 8 bytes");
static_assert(offsetof(linux_winsize, ws_row) == 0U);
static_assert(offsetof(linux_winsize, ws_col) == 2U);
static_assert(offsetof(linux_winsize, ws_xpixel) == 4U);
static_assert(offsetof(linux_winsize, ws_ypixel) == 6U);
