#pragma once
#include <cstdint>

namespace HAL {
    void init_platform();
    void send_uart_char(char c);
    void ack_interrupt(uint32_t irq_id);
}