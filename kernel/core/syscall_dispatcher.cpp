// core/syscall_dispatcher.hpp
#include <cpu_context.hpp>

enum class SyscallLinuxAArch64 : uint64_t {
    Write = 64,
    Exit  = 93,
};

class SyscallDispatcher {
public:
    static void handle_trap(CpuRegisters* regs) {
        auto syscall_num = static_cast<SyscallLinuxAArch64>(regs->x[8]);
        
        switch (syscall_num) {
            case SyscallLinuxAArch64::Write:
                // Route fd (x0), buf (x1), count (x2) to IPC or HAL output
                regs->x[0] = regs->x[2]; // Return bytes written (stub)
                break;
            case SyscallLinuxAArch64::Exit:
                // Schedule next thread
                break;
            default:
                regs->x[0] = -38; // -ENOSYS
                break;
        }
    }
};