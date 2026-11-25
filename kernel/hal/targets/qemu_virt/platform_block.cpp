#include "drivers/irq/gicv2.hpp"
#include "drivers/virtio/virtio_blk.hpp"
#include "hal/platform.hpp"

#include <cstddef>
#include <cstdint>

namespace {

// QEMU virt machine Virtio-MMIO bus layout:
// 32 slots at 0x0A000000 (stride 0x200), wired to GIC SPIs 16..47 (GIC IRQ IDs 48..79).
constexpr uintptr_t QEMU_VIRTIO_MMIO_BASE = 0x0A000000ULL;
constexpr size_t QEMU_VIRTIO_MMIO_STRIDE = 0x200U;
constexpr size_t QEMU_VIRTIO_MMIO_SLOTS = 32U;
constexpr uint32_t QEMU_VIRTIO_FIRST_SPI_IRQ = Kernel::Drivers::IRQ::MIN_SPI_IRQ + 16U;

Kernel::Drivers::Virtio::VirtioBlkDevice g_primary_virtio_blk{};
Kernel::Drivers::Block::BlockDevice* g_primary_block_dev = nullptr;

} // namespace

namespace HAL::Platform {

void init_block_devices(uintptr_t dtb_paddr) noexcept {
    (void)dtb_paddr;

    if (g_primary_block_dev != nullptr) {
        return;
    }

    for (size_t slot = 0U; slot < QEMU_VIRTIO_MMIO_SLOTS; ++slot) {
        const uintptr_t mmio_base = QEMU_VIRTIO_MMIO_BASE + (slot * QEMU_VIRTIO_MMIO_STRIDE);
        const uint32_t spi_irq = QEMU_VIRTIO_FIRST_SPI_IRQ + static_cast<uint32_t>(slot);

        // VirtioBlkDevice::init() probes the slot, verifies VIRTIO_DEV_ID_BLK,
        // reads sector capacity from config offset 0x100, registers the GICv2 SPI
        // handler via Kernel::Drivers::IRQ::GicV2::register_spi_handler(), and sets DRIVER_OK.
        if (g_primary_virtio_blk.init(mmio_base, spi_irq)) {
            g_primary_block_dev = &g_primary_virtio_blk;
            return;
        }
    }
}

Kernel::Drivers::Block::BlockDevice* primary_block_device() noexcept {
    return g_primary_block_dev;
}

} // namespace HAL::Platform