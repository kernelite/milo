#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef HOST_TEST
#undef SIG_BLOCK
#undef SIG_UNBLOCK
#undef SIG_SETMASK
#undef SIG_DFL
#undef SIG_IGN
#endif

namespace Kernel::UAPI {

inline constexpr size_t MAX_SIGNALS = 64U;

// Standard Linux AArch64 signal numbers
inline constexpr uint32_t SIG_HUP = 1U;
inline constexpr uint32_t SIG_INT = 2U;
inline constexpr uint32_t SIG_QUIT = 3U;
inline constexpr uint32_t SIG_KILL = 9U;
inline constexpr uint32_t SIG_PIPE = 13U;
inline constexpr uint32_t SIG_ALRM = 14U;
inline constexpr uint32_t SIG_TERM = 15U;
inline constexpr uint32_t SIG_CHLD = 17U;
inline constexpr uint32_t SIG_CONT = 18U;
inline constexpr uint32_t SIG_STOP = 19U;
inline constexpr uint32_t SIG_TSTP = 20U;
inline constexpr uint32_t SIG_TTIN = 21U;
inline constexpr uint32_t SIG_TTOU = 22U;
inline constexpr uint64_t SIGCHLD_NUM = 17ULL;

// Signal disposition & sigaction flags
inline constexpr uint64_t SIG_DFL = 0ULL;
inline constexpr uint64_t SIG_IGN = 1ULL;
inline constexpr uint64_t SA_FLAG_RESTORER = 0x04000000ULL;

// rt_sigprocmask() how selectors & signal bitmasks
inline constexpr uint64_t SIG_BLOCK = 0ULL;
inline constexpr uint64_t SIG_UNBLOCK = 1ULL;
inline constexpr uint64_t SIG_SETMASK = 2ULL;

inline constexpr uint64_t SIG_UNMASKABLE_MASK =
    (1ULL << (SIG_KILL - 1U)) | (1ULL << (SIG_STOP - 1U));
inline constexpr uint64_t SIG_STOP_MASK = (1ULL << (SIG_STOP - 1U)) | (1ULL << (SIG_TSTP - 1U)) |
                                          (1ULL << (SIG_TTIN - 1U)) | (1ULL << (SIG_TTOU - 1U));

// EL0 signal return frame magic ("SIGFRAME")
inline constexpr uint64_t RT_SIGFRAME_MAGIC = 0x5349474652414D45ULL;

} // namespace Kernel::UAPI

extern "C" struct linux_sigaction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};
static_assert(std::is_standard_layout_v<linux_sigaction>,
              "linux_sigaction must be standard layout");
static_assert(std::is_trivial_v<linux_sigaction>, "linux_sigaction must be trivial");
static_assert(sizeof(linux_sigaction) == 32U, "linux_sigaction must be 32 bytes");
static_assert(offsetof(linux_sigaction, sa_handler) == 0U);
static_assert(offsetof(linux_sigaction, sa_flags) == 8U);
static_assert(offsetof(linux_sigaction, sa_restorer) == 16U);
static_assert(offsetof(linux_sigaction, sa_mask) == 24U);

extern "C" struct linux_sigset_argpack {
    uint64_t ss;
    uint64_t ss_len;
};
static_assert(std::is_standard_layout_v<linux_sigset_argpack>,
              "linux_sigset_argpack must be standard layout");
static_assert(std::is_trivial_v<linux_sigset_argpack>, "linux_sigset_argpack must be trivial");
static_assert(sizeof(linux_sigset_argpack) == 16U, "linux_sigset_argpack must be 16 bytes");
static_assert(offsetof(linux_sigset_argpack, ss) == 0U);
static_assert(offsetof(linux_sigset_argpack, ss_len) == 8U);

// Linux AArch64 rt_sigframe layout pushed onto EL0 user stack
extern "C" struct alignas(16) linux_rt_sigframe {
    uint64_t magic;
    uint64_t saved_mask;
    uint64_t regs[31];
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
    uint64_t tpidr_el0;
    // 2-instruction return trampoline: mov x8, #139 (SYS_RT_SIGRETURN); svc #0
    uint32_t trampoline[2];
    uint64_t pad;
};
static_assert(std::is_standard_layout_v<linux_rt_sigframe>,
              "linux_rt_sigframe must be standard layout");
static_assert(std::is_trivial_v<linux_rt_sigframe>, "linux_rt_sigframe must be trivial");
static_assert(sizeof(linux_rt_sigframe) == 320U, "linux_rt_sigframe must be 320 bytes");
static_assert((sizeof(linux_rt_sigframe) % 16U) == 0U, "linux_rt_sigframe must be 16-byte aligned");
static_assert(offsetof(linux_rt_sigframe, magic) == 0U);
static_assert(offsetof(linux_rt_sigframe, saved_mask) == 8U);
static_assert(offsetof(linux_rt_sigframe, regs) == 16U);
static_assert(offsetof(linux_rt_sigframe, sp_el0) == 264U);
static_assert(offsetof(linux_rt_sigframe, elr_el1) == 272U);
static_assert(offsetof(linux_rt_sigframe, spsr_el1) == 280U);
static_assert(offsetof(linux_rt_sigframe, tpidr_el0) == 288U);
static_assert(offsetof(linux_rt_sigframe, trampoline) == 296U);
