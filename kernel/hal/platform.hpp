#pragma once
#include <cstdint>

namespace HAL {
void init_platform();
void send_uart_char(char chr);
void ack_interrupt(uint32_t irq_id);
} // namespace HAL