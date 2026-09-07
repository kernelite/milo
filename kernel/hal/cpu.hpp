#pragma once
#include <cstdint>

namespace HAL {

// Architectural Register Save Frame
struct CpuContext {
    uint64_t x[31];     // General Purpose Registers x0-x30
    uint64_t sp_el0;    // User Stack Pointer
    uint64_t elr_el1;   // Return Program Counter
    uint64_t spsr_el1;  // Saved Program Status Register
};

class CpuControl {
public:
    constexpr CpuControl() = default;
    virtual ~CpuControl() = default;

    // Interrupt Controls
    virtual void enable_interrupts() = 0;
    virtual void disable_interrupts() = 0;
    virtual bool interrupts_enabled() const = 0;

    // Low-Power State
    virtual void halt() = 0; // Wait for Interrupt (WFI)

    // Context Switch
    virtual void switch_context(CpuContext** old_ctx, CpuContext* new_ctx) = 0;
};

extern CpuControl& cpu;
}