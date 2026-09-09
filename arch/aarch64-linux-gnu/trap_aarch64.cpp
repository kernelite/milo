#include <cstddef>
#include <cstdint>
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/trap.hpp"

extern "C" {
void aarch64_handle_sync_exception(HAL::CpuContext* ctx, uint64_t esr, uint64_t far) noexcept;
void aarch64_handle_invalid_exception(HAL::CpuContext* ctx, uint64_t type) noexcept;
[[noreturn]] void return_to_kernel(int64_t exit_code) noexcept;
}

namespace {

void print_str(const char* str) noexcept {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    HAL::get_console().write(str, len);
}

void print_hex64(uint64_t val) noexcept {
    char buf[19] = "0x0000000000000000";
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 17; i >= 2; --i) {
        buf[i] = hex_chars[val & 0xFU];
        val >>= 4U;
    }
    print_str(buf);
}

void dump_registers(const HAL::CpuContext* ctx) noexcept {
    print_str("\r\n================ CPU REGISTER DUMP ================\r\n");
    for (int i = 0; i < 31; i += 2) {
        print_str("  X");
        if (i < 10) {
            print_str("0");
        }
        HAL::get_console().putc(static_cast<char>('0' + (i % 10)));
        print_str(": ");
        print_hex64(ctx->x[i]);
        print_str("   ");

        if (i + 1 < 31) {
            print_str("X");
            if (i + 1 < 10) {
                print_str("0");
            }
            HAL::get_console().putc(static_cast<char>('0' + ((i + 1) % 10)));
            print_str(": ");
            print_hex64(ctx->x[i + 1]);
        }
        print_str("\r\n");
    }
    print_str("  SP_EL0  : ");
    print_hex64(ctx->sp_el0);
    print_str("\r\n  ELR_EL1 : ");
    print_hex64(ctx->elr_el1);
    print_str("\r\n  SPSR_EL1: ");
    print_hex64(ctx->spsr_el1);
    print_str("\r\n===================================================\r\n");
}

class AArch64TrapHandler : public HAL::TrapHandler {
public:
    constexpr AArch64TrapHandler() = default;

    void handle_trap(HAL::TrapFrame& frame) override {
        (void)frame;
    }
};

constinit AArch64TrapHandler aarch64_trap_handler{};

} // namespace

namespace HAL {
TrapHandler& get_trap_dispatcher() noexcept {
    return aarch64_trap_handler;
}
} // namespace HAL

extern "C" {

void aarch64_handle_sync_exception(HAL::CpuContext* ctx, uint64_t esr, uint64_t far) noexcept {
    const uint32_t eclass = static_cast<uint32_t>((esr >> 26U) & 0x3FU); // Exception Class (EC)

    switch (eclass) {
    case 0x15U: // SVC in AArch64
    {
        const auto syscall_num = ctx->x[8];

        if (syscall_num == 64U) { // SYS_WRITE
            const auto filedesc = ctx->x[0];
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            const auto* const buf = reinterpret_cast<const char*>(ctx->x[1]);
            const auto count = ctx->x[2];
            if (filedesc == 1U || filedesc == 2U) {
                HAL::get_console().write(buf, count);
            }
            ctx->x[0] = count; // Return bytes written
        } else if (syscall_num == 93U) { // SYS_EXIT
            print_str("[TRAP] Process exited cleanly via SYS_EXIT (93).\r\n");
            const auto exit_status = static_cast<int64_t>(ctx->x[0]);
            return_to_kernel(exit_status);
        } else {
            ctx->x[0] = static_cast<uint64_t>(-38); // -ENOSYS
        }

        // Advance ELR_EL1 past the 4-byte SVC instruction to prevent re-triggering
        ctx->elr_el1 += 4U;
        return;
    }

    case 0x20U: // Instruction Abort from Lower EL
    case 0x21U: // Instruction Abort from Current EL
    case 0x24U: // Data Abort from Lower EL
    case 0x25U: // Data Abort from Current EL
    {
        print_str("\r\n[TRAP] Data/Instruction Abort Trap Captured!\r\n");
        print_str("  Faulting Virtual Address (FAR_EL1): ");
        print_hex64(far);
        print_str("\r\n  Syndrome Register        (ESR_EL1): ");
        print_hex64(esr);
        print_str("\r\n");

        dump_registers(ctx);

        if ((ctx->spsr_el1 & 0x0FU) == 0U) {
            print_str("[TRAP] Terminating faulting EL0 process.\r\n");
            return_to_kernel(-1);
        }

        print_str("[TRAP] EL1 Fault detected! Advancing ELR_EL1 past faulting instruction...\r\n");
        ctx->elr_el1 += 4U;
        return;
    }

    default: {
        print_str("\r\n[PANIC] Unhandled Synchronous Exception! EC=");
        print_hex64(eclass);
        print_str(" ESR_EL1=");
        print_hex64(esr);
        print_str(" FAR_EL1=");
        print_hex64(far);
        print_str("\r\n");

        dump_registers(ctx);

        if ((ctx->spsr_el1 & 0x0FU) == 0U) {
            print_str("[TRAP] Terminating faulting EL0 process.\r\n");
            return_to_kernel(-1);
        }

        while (true) {
            HAL::get_cpu().halt();
        }
    }
    }
}

void aarch64_handle_invalid_exception(HAL::CpuContext* ctx, uint64_t type) noexcept {
    constexpr const char* type_names[] = {
        "Unknown",
        "Current EL SP0 IRQ",
        "Current EL SP0 FIQ",
        "Current EL SP0 SError",
        "Unknown",
        "Current EL SPx IRQ",
        "Current EL SPx FIQ",
        "Current EL SPx SError",
        "Unknown",
        "Lower EL AArch64 IRQ",
        "Lower EL AArch64 FIQ",
        "Lower EL AArch64 SError",
        "Lower EL AArch32 Sync",
        "Lower EL AArch32 IRQ",
        "Lower EL AArch32 FIQ",
        "Lower EL AArch32 SError"
    };

    print_str("\r\n[PANIC] Invalid/Async Trap Captured: ");
    if (type < 16U) {
        print_str(type_names[type]);
    } else {
        print_hex64(type);
    }
    print_str("\r\n");

    dump_registers(ctx);

    if ((ctx->spsr_el1 & 0x0FU) == 0U) {
        print_str("[TRAP] Terminating faulting EL0 process.\r\n");
        return_to_kernel(-3);
    }

    while (true) {
        HAL::get_cpu().halt();
    }
}

} // extern "C"