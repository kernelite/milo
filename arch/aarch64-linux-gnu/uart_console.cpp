#include <hal/console.hpp>

namespace {
    constexpr uintptr_t UART0_BASE = 0x09000000;

    class UARTConsole : public HAL::Console {
    public:
        // Mark constructor as constexpr / default to allow static initialization
        constexpr UARTConsole() = default;

        void init() override {}

        void putc(char c) override {
            volatile uint32_t* const DR = reinterpret_cast<volatile uint32_t*>(UART0_BASE + 0x00);
            volatile uint32_t* const FR = reinterpret_cast<volatile uint32_t*>(UART0_BASE + 0x18);

            // Wait until Transmit FIFO is not full (bit 5)
            while (*FR & (1 << 5)) {}
            *DR = static_cast<uint32_t>(c);
        }

        uint8_t getc() override {
            volatile uint32_t* const DR = reinterpret_cast<volatile uint32_t*>(UART0_BASE + 0x00);
            volatile uint32_t* const FR = reinterpret_cast<volatile uint32_t*>(UART0_BASE + 0x18);

            // Wait until Receive FIFO is not empty (bit 4)
            while (*FR & (1 << 4)) {}
            return static_cast<uint8_t>(*DR & 0xFF);
        }
    };

    // Use constinit (C++20) to guarantee zero runtime constructor overhead
    constinit UARTConsole global_uart_driver{};
}

namespace HAL {
    Console& console = global_uart_driver;
}