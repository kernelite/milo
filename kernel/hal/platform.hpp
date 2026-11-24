#pragma once
#include "drivers/block/block_device.hpp"

#include <cstdint>

namespace HAL {
void init_platform();
void send_uart_char(char chr);
void ack_interrupt(uint32_t irq_id);
} // namespace HAL

namespace HAL::Platform {

// Discovers and initializes platform block storage devices and wires their
// interrupts into the platform interrupt controller.
void init_block_devices(uintptr_t dtb_paddr = 0U) noexcept;

// Returns the primary system BlockDevice, or nullptr if no block device is present.
[[nodiscard]] Kernel::Drivers::Block::BlockDevice* primary_block_device() noexcept;

} // namespace HAL::Platform