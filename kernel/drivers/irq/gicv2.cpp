#include "drivers/irq/gicv2.hpp"

#include "mm/AddressSpace.hpp"

namespace Kernel::Drivers::IRQ {

namespace {

constexpr size_t NUM_SPI_LINES = MAX_SPI_IRQ - MIN_SPI_IRQ + 1U;

constexpr uintptr_t GICD_CTLR_OFF = 0x000U;
constexpr uintptr_t GICD_TYPER_OFF = 0x004U;
constexpr uintptr_t GICD_IGROUPR_OFF = 0x080U;
constexpr uintptr_t GICD_ISENABLER_OFF = 0x100U;
constexpr uintptr_t GICD_ICENABLER_OFF = 0x180U;
constexpr uintptr_t GICD_ICPENDR_OFF = 0x280U;
constexpr uintptr_t GICD_IPRIORITYR_OFF = 0x400U;
constexpr uintptr_t GICD_ITARGETSR_OFF = 0x800U;
constexpr uintptr_t GICD_ICFGR_OFF = 0xC00U;

constexpr uintptr_t GICC_CTLR_OFF = 0x000U;
constexpr uintptr_t GICC_PMR_OFF = 0x004U;
constexpr uintptr_t GICC_IAR_OFF = 0x00CU;
constexpr uintptr_t GICC_EOIR_OFF = 0x010U;

struct SpiSlot {
    IrqHandler handler{nullptr};
    void* context{nullptr};
};

uintptr_t g_gicd_virt_base = 0U;
uintptr_t g_gicc_virt_base = 0U;
SpiSlot g_spi_handlers[NUM_SPI_LINES]{};

[[nodiscard]] uintptr_t gicd_base() noexcept {
    if (g_gicd_virt_base != 0U) {
        return g_gicd_virt_base;
    }
    return Kernel::MM::phys_to_virt(GICD_DEFAULT_BASE);
}

[[nodiscard]] uintptr_t gicc_base() noexcept {
    if (g_gicc_virt_base != 0U) {
        return g_gicc_virt_base;
    }
    return Kernel::MM::phys_to_virt(GICC_DEFAULT_BASE);
}

void write_byte_in_word(uintptr_t reg_addr, uint32_t byte_index, uint8_t value) noexcept {
    auto* const reg = reinterpret_cast<volatile uint32_t*>(reg_addr);
    const uint32_t shift = (byte_index & 0x3U) * 8U;
    const uint32_t mask = 0xFFU << shift;
    uint32_t current = *reg;
    current = (current & ~mask) | (static_cast<uint32_t>(value) << shift);
    *reg = current;
}

} // namespace

void GicV2::set_mmio_bases(uintptr_t gicd_virt_base, uintptr_t gicc_virt_base) noexcept {
    g_gicd_virt_base = gicd_virt_base;
    g_gicc_virt_base = gicc_virt_base;
}

void GicV2::reset_handlers() noexcept {
    for (auto& slot : g_spi_handlers) {
        slot = SpiSlot{};
    }
}

void GicV2::init() noexcept {
    const uintptr_t gicd = gicd_base();
    const uintptr_t gicc = gicc_base();

    auto* const gicd_ctlr = reinterpret_cast<volatile uint32_t*>(gicd + GICD_CTLR_OFF);
    *gicd_ctlr = 0U; // Disable distributor during configuration

    const uint32_t typer = *reinterpret_cast<volatile uint32_t*>(gicd + GICD_TYPER_OFF);
    const uint32_t it_lines = typer & 0x1FU;
    uint32_t max_lines = (it_lines + 1U) * 32U;
    if (max_lines < 256U) {
        max_lines = 256U;
    }
    if (max_lines > MAX_IRQ_LINES) {
        max_lines = MAX_IRQ_LINES;
    }

    // Route all SPIs (IRQs 32..max_lines) to CPU 0 by default
    for (uint32_t irq = MIN_SPI_IRQ; irq < max_lines; irq += 4U) {
        auto* const itargetsr =
            reinterpret_cast<volatile uint32_t*>(gicd + GICD_ITARGETSR_OFF + irq);
        *itargetsr = 0x01010101U;
    }

    // Set default priority (0x80) across all interrupt lines
    for (uint32_t irq = 0U; irq < max_lines; irq += 4U) {
        auto* const ipriorityr =
            reinterpret_cast<volatile uint32_t*>(gicd + GICD_IPRIORITYR_OFF + irq);
        *ipriorityr = 0x80808080U;
    }

    // Configure all interrupts in Group 0 and clear stale pending SPIs
    for (uint32_t irq = 0U; irq < max_lines; irq += 32U) {
        const uintptr_t bank_off = static_cast<uintptr_t>(irq / 32U) * 4U;
        auto* const igroupr =
            reinterpret_cast<volatile uint32_t*>(gicd + GICD_IGROUPR_OFF + bank_off);
        *igroupr = 0U;

        if (irq >= MIN_SPI_IRQ) {
            auto* const icpendr =
                reinterpret_cast<volatile uint32_t*>(gicd + GICD_ICPENDR_OFF + bank_off);
            *icpendr = 0xFFFFFFFFU;
        }
    }

    reset_handlers();

    *gicd_ctlr = 1U; // Enable distributor

    auto* const gicc_pmr = reinterpret_cast<volatile uint32_t*>(gicc + GICC_PMR_OFF);
    *gicc_pmr = 0xF0U; // Unmask all interrupt priority levels

    auto* const gicc_ctlr = reinterpret_cast<volatile uint32_t*>(gicc + GICC_CTLR_OFF);
    *gicc_ctlr = 1U; // Enable CPU interface
}

void GicV2::enable_irq(uint32_t irq) noexcept {
    if (irq >= MAX_IRQ_LINES) {
        return;
    }
    const uintptr_t gicd = gicd_base();
    auto* const isenabler = reinterpret_cast<volatile uint32_t*>(
        gicd + GICD_ISENABLER_OFF + (static_cast<uintptr_t>(irq / 32U) * 4U));
    *isenabler = 1U << (irq % 32U);
}

void GicV2::disable_irq(uint32_t irq) noexcept {
    if (irq >= MAX_IRQ_LINES) {
        return;
    }
    const uintptr_t gicd = gicd_base();
    auto* const icenabler = reinterpret_cast<volatile uint32_t*>(
        gicd + GICD_ICENABLER_OFF + (static_cast<uintptr_t>(irq / 32U) * 4U));
    *icenabler = 1U << (irq % 32U);
}

void GicV2::set_irq_priority(uint32_t irq, uint8_t priority) noexcept {
    if (irq >= MAX_IRQ_LINES) {
        return;
    }
    const uintptr_t gicd = gicd_base();
    const uintptr_t aligned_off = static_cast<uintptr_t>(irq & ~3U);
    write_byte_in_word(gicd + GICD_IPRIORITYR_OFF + aligned_off, irq & 3U, priority);
}

void GicV2::set_spi_target(uint32_t irq, uint8_t cpu_mask) noexcept {
    if (irq < MIN_SPI_IRQ || irq > MAX_SPI_IRQ) {
        return;
    }
    const uintptr_t gicd = gicd_base();
    const uintptr_t aligned_off = static_cast<uintptr_t>(irq & ~3U);
    write_byte_in_word(gicd + GICD_ITARGETSR_OFF + aligned_off, irq & 3U, cpu_mask);
}

void GicV2::set_irq_trigger(uint32_t irq, IrqTriggerMode mode) noexcept {
    if (irq < MIN_SPI_IRQ || irq > MAX_SPI_IRQ) {
        return;
    }
    const uintptr_t gicd = gicd_base();
    const uintptr_t reg_off = static_cast<uintptr_t>(irq / 16U) * 4U;
    auto* const icfgr = reinterpret_cast<volatile uint32_t*>(gicd + GICD_ICFGR_OFF + reg_off);

    const uint32_t bit_shift = (irq % 16U) * 2U;
    const uint32_t field_mask = 0x3U << bit_shift;
    const uint32_t encoded = (mode == IrqTriggerMode::EdgeRising) ? (0x2U << bit_shift) : 0U;

    uint32_t current = *icfgr;
    current = (current & ~field_mask) | encoded;
    *icfgr = current;
}

bool GicV2::register_spi_handler(uint32_t irq, IrqHandler handler, void* context,
                                 IrqTriggerMode trigger, uint8_t priority,
                                 uint8_t cpu_target_mask) noexcept {
    if (irq < MIN_SPI_IRQ || irq > MAX_SPI_IRQ || handler == nullptr || cpu_target_mask == 0U) {
        return false;
    }

    disable_irq(irq);
    set_spi_target(irq, cpu_target_mask);
    set_irq_priority(irq, priority);
    set_irq_trigger(irq, trigger);

    // Clear any stale pending interrupt before enabling
    const uintptr_t gicd = gicd_base();
    auto* const icpendr = reinterpret_cast<volatile uint32_t*>(
        gicd + GICD_ICPENDR_OFF + (static_cast<uintptr_t>(irq / 32U) * 4U));
    *icpendr = 1U << (irq % 32U);

    g_spi_handlers[irq - MIN_SPI_IRQ] = SpiSlot{
        .handler = handler,
        .context = context,
    };

    enable_irq(irq);
    return true;
}

bool GicV2::unregister_spi_handler(uint32_t irq) noexcept {
    if (irq < MIN_SPI_IRQ || irq > MAX_SPI_IRQ) {
        return false;
    }
    disable_irq(irq);
    g_spi_handlers[irq - MIN_SPI_IRQ] = SpiSlot{};
    return true;
}

bool GicV2::has_handler(uint32_t irq) noexcept {
    if (irq < MIN_SPI_IRQ || irq > MAX_SPI_IRQ) {
        return false;
    }
    return g_spi_handlers[irq - MIN_SPI_IRQ].handler != nullptr;
}

bool GicV2::dispatch_irq(uint32_t irq) noexcept {
    if (irq < MIN_SPI_IRQ || irq > MAX_SPI_IRQ) {
        return false;
    }
    const SpiSlot slot = g_spi_handlers[irq - MIN_SPI_IRQ];
    if (slot.handler == nullptr) {
        return false;
    }
    slot.handler(irq, slot.context);
    return true;
}

uint32_t GicV2::acknowledge_irq() noexcept {
    const uintptr_t gicc = gicc_base();
    auto* const gicc_iar = reinterpret_cast<volatile uint32_t*>(gicc + GICC_IAR_OFF);
    return *gicc_iar & 0x3FFU;
}

void GicV2::end_of_interrupt(uint32_t irq) noexcept {
    const uintptr_t gicc = gicc_base();
    auto* const gicc_eoir = reinterpret_cast<volatile uint32_t*>(gicc + GICC_EOIR_OFF);
    *gicc_eoir = irq;
}

} // namespace Kernel::Drivers::IRQ