#include <hal/console.hpp>

namespace {
constexpr uintptr_t UART0_BASE = 0x09000000;

class UARTConsole : public HAL::Console {
  public:
    // Mark constructor as constexpr / default to allow static initialization
    constexpr UARTConsole() = default;

    void init() override {}

    void putc(char chr) override {
        volatile uint32_t* const DataReg = reinterpret_cast<volatile uint32_t*>(UART0_BASE + 0x00);
        volatile uint32_t* const FlagReg = reinterpret_cast<volatile uint32_t*>(UART0_BASE + 0x18);

        // Explicit comparison with != 0U satisfies readability-implicit-bool-conversion
        while ((*FlagReg & (1U << 5)) != 0U) {
        }
        *DataReg = static_cast<uint32_t>(chr);
    }

    uint8_t getc() override {
        volatile uint32_t* const DataReg = reinterpret_cast<volatile uint32_t*>(UART0_BASE + 0x00);
        volatile uint32_t* const FlagReg = reinterpret_cast<volatile uint32_t*>(UART0_BASE + 0x18);

        // Wait until Receive FIFO is not empty (bit 4)
        while ((*FlagReg & (1U << 4)) != 0U) {
        }
        return static_cast<uint8_t>(*DataReg & 0xFFU);
    }
};

// Use constinit (C++20) to guarantee zero runtime constructor overhead
constinit UARTConsole global_uart_driver{};
} // namespace

namespace HAL {
Console& get_console() noexcept {
    return global_uart_driver;
}
} // namespace HAL