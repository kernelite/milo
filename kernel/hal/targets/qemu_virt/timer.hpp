#pragma once

#include <cstdint>

namespace HAL::QemuVirt {

class GenericTimer {
  public:
    static void init(uint32_t freq_hz) noexcept;
    static void disable() noexcept;
    static void reload() noexcept;
    static uint64_t frequency_hz() noexcept;
    static uint64_t counter_ticks() noexcept;
};

} // namespace HAL::QemuVirt