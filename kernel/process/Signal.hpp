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

class SignalTable final {
  public:
    static constexpr uint64_t UNMASKABLE_MASK =
        (1ULL << (SIG_KILL - 1U)) | (1ULL << (SIG_STOP - 1U));
    static constexpr uint64_t STOP_SIGNALS_MASK =
        (1ULL << (SIG_STOP - 1U)) | (1ULL << (SIG_TSTP - 1U)) | (1ULL << (SIG_TTIN - 1U)) |
        (1ULL << (SIG_TTOU - 1U));

    constexpr SignalTable() noexcept = default;

    void pend(uint32_t sig) noexcept;
    void clear_stop_signals() noexcept;
    [[nodiscard]] bool dequeue_next(uint32_t& out_sig) noexcept;

    [[nodiscard]] bool has_unblocked_pending() const noexcept;
    [[nodiscard]] bool is_deliverable_for_wakeup(uint32_t sig) const noexcept;

    [[nodiscard]] constexpr uint64_t blocked() const noexcept { return m_blocked; }
    void block(uint64_t mask) noexcept;
    void unblock(uint64_t mask) noexcept;
    void set_blocked(uint64_t mask) noexcept;

    [[nodiscard]] uint64_t swap_temp_mask(uint64_t temp_mask) noexcept;
    void restore_saved_mask_if_any() noexcept;
    void preserve_saved_mask_for_eintr(uint64_t saved) noexcept;
    [[nodiscard]] uint64_t consume_saved_or_blocked_mask() noexcept;

    [[nodiscard]] const SigAction& action(uint32_t sig) const noexcept;
    [[nodiscard]] bool set_action(uint32_t sig, const SigAction& act) noexcept;
    void reset_caught_for_exec() noexcept;
    void clone_from(const SignalTable& src) noexcept;

  private:
    [[nodiscard]] static constexpr bool is_valid_signal(uint32_t sig) noexcept {
        return sig > 0U && sig <= MAX_SIGNALS;
    }

    [[nodiscard]] static constexpr uint64_t sig_bit(uint32_t sig) noexcept {
        return 1ULL << (sig - 1U);
    }

    uint64_t m_pending{0ULL};
    uint64_t m_blocked{0ULL};
    uint64_t m_saved_mask{0ULL};
    SigAction m_actions[MAX_SIGNALS]{};
    bool m_has_saved_mask{false};
};

class Process;

// Signal delivery & sigreturn unwinding
void send_signal(Process& target, uint32_t sig) noexcept;
void signal_process_group(uint32_t pgid, uint32_t sig) noexcept;
[[nodiscard]] bool has_unblocked_pending_signal(const Process& proc) noexcept;
void deliver_pending_signals(HAL::CpuContext& ctx) noexcept;
int64_t restore_signal_frame(HAL::CpuContext& ctx) noexcept;

} // namespace Kernel::Process
