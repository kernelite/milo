#pragma once
#include <cstddef>
#include <cstdint>

namespace HAL {
class Console {
  public:
    virtual void init() = 0;
    virtual void putc(char chr) = 0;
    virtual uint8_t getc() = 0;

    // Non-virtual inline helper calling abstract putc
    void write(const char* str, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            putc(str[i]);
        }
    }
};

Console& get_console() noexcept;

} // namespace HAL