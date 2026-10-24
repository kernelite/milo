#include "hal/console.hpp"
#include "hal/trap.hpp"

namespace Kernel {

class SyscallDispatcher : public HAL::TrapHandler {
  public:
    constexpr SyscallDispatcher() = default;

    void handle_trap(HAL::TrapFrame& frame) override {
        if (frame.type != HAL::TrapType::Syscall || frame.context == nullptr) {
            return;
        }

        auto* ctx = frame.context;
        const auto syscall_num = ctx->x[8];

        if (syscall_num == 64U) { // SYS_WRITE
            const auto filedesc = ctx->x[0];
            const auto* const buf = reinterpret_cast<const char*>(ctx->x[1]);
            const auto count = ctx->x[2];

            if (filedesc == 1U || filedesc == 2U) {
                HAL::get_console().write(buf, count);
            }
            ctx->x[0] = count;
        } else if (syscall_num == 93U) { // SYS_EXIT
            const auto exit_status = static_cast<int64_t>(ctx->x[0]);
            HAL::get_console().write("[SYSCALL] Process exited cleanly.\r\n", 35);
            // Schedule next process thread or exit
        } else {
            ctx->x[0] = static_cast<uint64_t>(-38); // -ENOSYS
        }
    }
};

constinit SyscallDispatcher global_syscall_dispatcher{};

} // namespace Kernel

namespace HAL {
TrapHandler& get_trap_dispatcher() noexcept {
    return Kernel::global_syscall_dispatcher;
}
} // namespace HAL