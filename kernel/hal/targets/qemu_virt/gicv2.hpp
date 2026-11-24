#pragma once

#include "drivers/irq/gicv2.hpp"

namespace HAL::QemuVirt {

constexpr uintptr_t GICD_BASE = Kernel::Drivers::IRQ::GICD_DEFAULT_BASE;
constexpr uintptr_t GICC_BASE = Kernel::Drivers::IRQ::GICC_DEFAULT_BASE;
constexpr uint32_t TIMER_PPI_IRQ = Kernel::Drivers::IRQ::TIMER_PPI_IRQ;

using GicV2 = Kernel::Drivers::IRQ::GicV2;

} // namespace HAL::QemuVirt