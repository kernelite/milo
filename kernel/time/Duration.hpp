#pragma once

#include "uapi/time.hpp"

#include <cstdint>

namespace Kernel::Time {

class Duration {
  public:
    static constexpr uint64_t NS_PER_USEC = 1000ULL;
    static constexpr uint64_t NS_PER_SEC = 1000000000ULL;

    constexpr Duration() noexcept = default;

    [[nodiscard]] static constexpr Duration zero() noexcept { return Duration(0ULL); }

    [[nodiscard]] static constexpr Duration from_nanoseconds(uint64_t nanos) noexcept {
        return Duration(nanos);
    }

    [[nodiscard]] static constexpr Duration from_microseconds(uint64_t micros) noexcept {
        return Duration(micros * NS_PER_USEC);
    }

    [[nodiscard]] static constexpr Duration from_ticks(uint64_t ticks, uint64_t freq_hz) noexcept {
        if (freq_hz == 0ULL || ticks == 0ULL) {
            return zero();
        }
        const uint64_t secs = ticks / freq_hz;
        const uint64_t rem_ticks = ticks % freq_hz;
        return Duration((secs * NS_PER_SEC) + ((rem_ticks * NS_PER_SEC) / freq_hz));
    }

    [[nodiscard]] static constexpr bool from_linux_timespec(const linux_timespec& time_spec,
                                                            Duration& out) noexcept {
        if (time_spec.tv_sec < 0 || time_spec.tv_nsec < 0 ||
            time_spec.tv_nsec >= static_cast<int64_t>(NS_PER_SEC)) {
            return false;
        }
        const auto secs = static_cast<uint64_t>(time_spec.tv_sec);
        const auto nsec = static_cast<uint64_t>(time_spec.tv_nsec);
        if (secs > ((UINT64_MAX - nsec) / NS_PER_SEC)) {
            return false;
        }
        out = Duration((secs * NS_PER_SEC) + nsec);
        return true;
    }

    [[nodiscard]] constexpr linux_timespec to_linux_timespec() const noexcept {
        return linux_timespec{
            .tv_sec = static_cast<int64_t>(m_ns / NS_PER_SEC),
            .tv_nsec = static_cast<int64_t>(m_ns % NS_PER_SEC),
        };
    }

    [[nodiscard]] constexpr linux_timeval to_linux_timeval() const noexcept {
        return linux_timeval{
            .tv_sec = static_cast<int64_t>(m_ns / NS_PER_SEC),
            .tv_usec = static_cast<int64_t>((m_ns % NS_PER_SEC) / NS_PER_USEC),
        };
    }

    [[nodiscard]] constexpr uint64_t to_timer_ticks(uint64_t freq_hz) const noexcept {
        if (freq_hz == 0ULL || m_ns == 0ULL) {
            return 0ULL;
        }
        const uint64_t secs = m_ns / NS_PER_SEC;
        const uint64_t rem_ns = m_ns % NS_PER_SEC;
        const uint64_t sec_ticks = secs * freq_hz;
        const uint64_t nsec_ticks = ((rem_ns * freq_hz) + (NS_PER_SEC - 1ULL)) / NS_PER_SEC;
        return sec_ticks + nsec_ticks;
    }

    [[nodiscard]] constexpr uint64_t nanoseconds() const noexcept { return m_ns; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return m_ns == 0ULL; }

  private:
    explicit constexpr Duration(uint64_t nanos) noexcept : m_ns(nanos) {}

    uint64_t m_ns{0ULL};
};

} // namespace Kernel::Time
