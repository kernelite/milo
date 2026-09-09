#pragma once
#include "cpu.hpp"

namespace HAL {

enum class TrapType { Syscall, DataAbort, InstructionAbort, Interrupt, Unknown };

struct TrapFrame {
    TrapType type;
    uint64_t error_code;
    CpuContext* context;
};

class TrapHandler {
  public:
    virtual ~TrapHandler() = default;
    virtual void handle_trap(TrapFrame& frame) = 0;
};

extern TrapHandler& trap_dispatcher;
} // namespace HAL