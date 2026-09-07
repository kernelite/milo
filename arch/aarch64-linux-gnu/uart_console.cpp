#include "../../kernel/hal/console.hpp"

namespace {
    class UARTConsole : public HAL::Console {
    private:
        // QEMU ARM64 'virt' board UART0 base address
        volatile uint32_t* const UART0_DR = reinterpret_cast<volatile uint32_t*>(0x09000000);

    public:
        void init() override {
            // Hardware baud rate / clock setup
        }

        void putc(char c) override {
            if (c == '\n') putc('\r');
            *UART0_DR = static_cast<uint32_t>(c);
        }

        uint8_t getc() override {
            return static_cast<uint8_t>(*UART0_DR & 0xFF);
        }
    };

    // Instantiate driver statically to avoid dynamic memory allocation
    UARTConsole g_uart_driver;
}

namespace HAL {
    // Bind the global HAL reference to the concrete hardware instance
    Console& console = g_uart_driver;
}