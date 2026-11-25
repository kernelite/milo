#pragma once
#include "cpu.hpp"

namespace HAL {

enum class TrapType : uint8_t { Syscall, DataAbort, InstructionAbort, Interrupt, Unknown };

struct TrapFrame {
    TrapType type;
    uint64_t error_code;
    CpuContext* context;
};

class TrapHandler {
  public:
    constexpr TrapHandler() = default;
    virtual ~TrapHandler() = default;

    TrapHandler(const TrapHandler&) = delete;
    TrapHandler& operator=(const TrapHandler&) = delete;
    TrapHandler(TrapHandler&&) = delete;
    TrapHandler& operator=(TrapHandler&&) = delete;

    virtual void handle_trap(TrapFrame& frame) = 0;
};

// Function accessor replacing global reference to prevent SIOF warnings
TrapHandler& get_trap_dispatcher() noexcept;

} // namespace HAL