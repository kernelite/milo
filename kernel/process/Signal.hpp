#pragma once

#include "hal/cpu.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Process {

constexpr size_t MAX_SIGNALS = 64U;

// Standard Linux Signal Numbers
constexpr uint32_t SIG_HUP = 1U;
constexpr uint32_t SIG_INT = 2U;
constexpr uint32_t SIG_QUIT = 3U;
constexpr uint32_t SIG_KILL = 9U;
constexpr uint32_t SIG_PIPE = 13U;
constexpr uint32_t SIG_ALRM = 14U;
constexpr uint32_t SIG_TERM = 15U;
constexpr uint32_t SIG_CHLD = 17U;
constexpr uint32_t SIG_CONT = 18U;
constexpr uint32_t SIG_STOP = 19U;
constexpr uint32_t SIG_TSTP = 20U;
constexpr uint32_t SIG_TTIN = 21U;
constexpr uint32_t SIG_TTOU = 22U;

constexpr uint64_t KERNEL_SIG_DFL = 0ULL;
constexpr uint64_t KERNEL_SIG_IGN = 1ULL;
constexpr uint64_t SA_FLAG_RESTORER = 0x04000000ULL;

struct SigAction {
    uint64_t handler{KERNEL_SIG_DFL};
    uint64_t flags{0ULL};
    uint64_t restorer{0ULL};
    uint64_t mask{0ULL};
};

struct Process;

// Signal delivery & sigreturn unwinding
void send_signal(Process& target, uint32_t sig) noexcept;
void signal_process_group(uint32_t pgid, uint32_t sig) noexcept;
[[nodiscard]] bool has_unblocked_pending_signal(const Process& proc) noexcept;
void deliver_pending_signals(HAL::CpuContext& ctx) noexcept;
int64_t restore_signal_frame(HAL::CpuContext& ctx) noexcept;

} // namespace Kernel::Process
