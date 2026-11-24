#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Drivers::IRQ {

constexpr uintptr_t GICD_DEFAULT_BASE = 0x08000000ULL;
constexpr uintptr_t GICC_DEFAULT_BASE = 0x08010000ULL;
constexpr uint32_t TIMER_PPI_IRQ = 30U;
constexpr uint32_t MIN_SPI_IRQ = 32U;
constexpr uint32_t MAX_SPI_IRQ = 1019U;
constexpr uint32_t MAX_IRQ_LINES = 1020U;
constexpr uint8_t DEFAULT_IRQ_PRIORITY = 0x80U;
constexpr uint8_t DEFAULT_CPU_TARGET_MASK = 0x01U;

enum class IrqTriggerMode : uint8_t {
    LevelHigh = 0U,
    EdgeRising = 1U,
};

using IrqHandler = void (*)(uint32_t irq, void* context) noexcept;

class GicV2 {
  public:
    static void init() noexcept;
    static void set_mmio_bases(uintptr_t gicd_virt_base, uintptr_t gicc_virt_base) noexcept;
    static void reset_handlers() noexcept;

    static void enable_irq(uint32_t irq) noexcept;
    static void disable_irq(uint32_t irq) noexcept;
    static void set_irq_priority(uint32_t irq, uint8_t priority) noexcept;
    static void set_spi_target(uint32_t irq, uint8_t cpu_mask) noexcept;
    static void set_irq_trigger(uint32_t irq, IrqTriggerMode mode) noexcept;

    [[nodiscard]] static bool
    register_spi_handler(uint32_t irq, IrqHandler handler, void* context = nullptr,
                         IrqTriggerMode trigger = IrqTriggerMode::LevelHigh,
                         uint8_t priority = DEFAULT_IRQ_PRIORITY,
                         uint8_t cpu_target_mask = DEFAULT_CPU_TARGET_MASK) noexcept;
    static bool unregister_spi_handler(uint32_t irq) noexcept;
    [[nodiscard]] static bool has_handler(uint32_t irq) noexcept;
    static bool dispatch_irq(uint32_t irq) noexcept;

    [[nodiscard]] static uint32_t acknowledge_irq() noexcept;
    static void end_of_interrupt(uint32_t irq) noexcept;
};

} // namespace Kernel::Drivers::IRQ