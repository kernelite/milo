#pragma once

#include <cstdint>

namespace HAL::QemuVirt {

constexpr uintptr_t GICD_BASE = 0x08000000ULL;
constexpr uintptr_t GICC_BASE = 0x08010000ULL;
constexpr uint32_t TIMER_PPI_IRQ = 30U;

class GicV2 {
  public:
    static void init() noexcept;
    static void enable_irq(uint32_t irq) noexcept;
    static void disable_irq(uint32_t irq) noexcept;
    static uint32_t acknowledge_irq() noexcept;
    static void end_of_interrupt(uint32_t irq) noexcept;
};

} // namespace HAL::QemuVirt