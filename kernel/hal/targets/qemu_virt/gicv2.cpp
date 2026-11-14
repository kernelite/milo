#include "hal/targets/qemu_virt/gicv2.hpp"

#include "mm/address_space.hpp"

namespace HAL::QemuVirt {

void GicV2::init() noexcept {
    const uintptr_t gicd = Kernel::MM::phys_to_virt(GICD_BASE);
    const uintptr_t gicc = Kernel::MM::phys_to_virt(GICC_BASE);

    auto* const gicd_ctlr = reinterpret_cast<volatile uint32_t*>(gicd + 0x000U);
    *gicd_ctlr = 0U; // Disable distributor during configuration

    // Route all SPIs (IRQs 32..255) to CPU 0
    for (uint32_t irq = 32U; irq < 256U; irq += 4U) {
        auto* const gicd_itargetsr = reinterpret_cast<volatile uint32_t*>(gicd + 0x800U + irq);
        *gicd_itargetsr = 0x01010101U;
    }

    // Configure all interrupts in Group 0
    for (uint32_t irq = 0U; irq < 256U; irq += 32U) {
        auto* const gicd_igroupr =
            reinterpret_cast<volatile uint32_t*>(gicd + 0x080U + ((irq / 32U) * 4U));
        *gicd_igroupr = 0U;
    }

    *gicd_ctlr = 1U; // Enable distributor

    auto* const gicc_pmr = reinterpret_cast<volatile uint32_t*>(gicc + 0x004U);
    *gicc_pmr = 0xF0U; // Unmask all interrupt priority levels

    auto* const gicc_ctlr = reinterpret_cast<volatile uint32_t*>(gicc + 0x000U);
    *gicc_ctlr = 1U; // Enable CPU interface
}

void GicV2::enable_irq(uint32_t irq) noexcept {
    const uintptr_t gicd = Kernel::MM::phys_to_virt(GICD_BASE);
    auto* const gicd_isenabler =
        reinterpret_cast<volatile uint32_t*>(gicd + 0x100U + ((irq / 32U) * 4U));
    *gicd_isenabler = 1U << (irq % 32U);
}

void GicV2::disable_irq(uint32_t irq) noexcept {
    const uintptr_t gicd = Kernel::MM::phys_to_virt(GICD_BASE);
    auto* const gicd_icenabler =
        reinterpret_cast<volatile uint32_t*>(gicd + 0x180U + ((irq / 32U) * 4U));
    *gicd_icenabler = 1U << (irq % 32U);
}

uint32_t GicV2::acknowledge_irq() noexcept {
    const uintptr_t gicc = Kernel::MM::phys_to_virt(GICC_BASE);
    auto* const gicc_iar = reinterpret_cast<volatile uint32_t*>(gicc + 0x00CU);
    return *gicc_iar & 0x3FFU;
}

void GicV2::end_of_interrupt(uint32_t irq) noexcept {
    const uintptr_t gicc = Kernel::MM::phys_to_virt(GICC_BASE);
    auto* const gicc_eoir = reinterpret_cast<volatile uint32_t*>(gicc + 0x010U);
    *gicc_eoir = irq;
}

} // namespace HAL::QemuVirt