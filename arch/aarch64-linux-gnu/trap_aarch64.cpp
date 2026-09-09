#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "cpu_context.hpp"

extern "C" {
    void aarch64_handle_sync_exception(HAL::CpuContext* ctx, uint64_t esr, uint64_t far);
    void aarch64_handle_invalid_exception(HAL::CpuContext* ctx, uint64_t type);
    [[noreturn]] void return_to_kernel(int64_t exit_code);
}

namespace {

void print_str(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') len++;
    HAL::console.write(str, len);
}

void print_hex64(uint64_t val) {
    char buf[19] = "0x0000000000000000";
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 17; i >= 2; --i) {
        buf[i] = hex_chars[val & 0xF];
        val >>= 4;
    }
    print_str(buf);
}

void dump_registers(HAL::CpuContext* ctx) {
    print_str("\r\n================ CPU REGISTER DUMP ================\r\n");
    for (int i = 0; i < 31; i += 2) {
        print_str("  X");
        if (i < 10) print_str("0");
        HAL::console.putc('0' + (i % 10));
        print_str(": ");
        print_hex64(ctx->x[i]);
        print_str("   ");

        if (i + 1 < 31) {
            print_str("X");
            if (i + 1 < 10) print_str("0");
            HAL::console.putc('0' + ((i + 1) % 10));
            print_str(": ");
            print_hex64(ctx->x[i + 1]);
        }
        print_str("\r\n");
    }
    print_str("  SP_EL0  : "); print_hex64(ctx->sp_el0);   print_str("\r\n");
    print_str("  ELR_EL1 : "); print_hex64(ctx->elr_el1);  print_str("\r\n");
    print_str("  SPSR_EL1: "); print_hex64(ctx->spsr_el1); print_str("\r\n");
    print_str("===================================================\r\n");
}

} // namespace

extern "C" void aarch64_handle_sync_exception(HAL::CpuContext* ctx, uint64_t esr, uint64_t far) {
    uint32_t ec = (esr >> 26) & 0x3F; // Extract Exception Class (EC)

    switch (ec) {
        case 0x15: // SVC in AArch64
        {
            auto syscall_num = ctx->x[8];

            if (syscall_num == 64) { // SYS_WRITE
                auto fd = ctx->x[0];
                auto buf = reinterpret_cast<const char*>(ctx->x[1]);
                auto count = ctx->x[2];
                if (fd == 1 || fd == 2) {
                    HAL::console.write(buf, count);
                }
                ctx->x[0] = count; // Return bytes written
                break;
            } else if (syscall_num == 93) { // SYS_EXIT
                print_str("[TRAP] Process exited cleanly via SYS_EXIT (93).\r\n");
                int64_t exit_status = static_cast<int64_t>(ctx->x[0]);
                // Restore kernel_ctx and return to enter_user_mode caller
                return_to_kernel(exit_status);
            } else {
                ctx->x[0] = static_cast<uint64_t>(-38); // -ENOSYS
                break;
            }
        }

        case 0x20: // Instruction Abort from Lower EL
        case 0x21: // Instruction Abort from Current EL
        case 0x24: // Data Abort from Lower EL
        case 0x25: // Data Abort from Current EL
        {
            print_str("\r\n[TRAP] Data/Instruction Abort Trap Captured!\r\n");
            print_str("  Faulting Virtual Address (FAR_EL1): ");
            print_hex64(far);
            print_str("\r\n  Syndrome Register        (ESR_EL1): ");
            print_hex64(esr);
            print_str("\r\n");

            dump_registers(ctx);

            // If the fault originated in EL0 (User Mode), terminate user process
            if ((ctx->spsr_el1 & 0x0F) == 0) {
                print_str("[TRAP] Terminating faulting EL0 process.\r\n");
                return_to_kernel(-1);
            }

            // If the fault originated in EL1 (Kernel Mode), advance ELR_EL1 to recover
            print_str("[TRAP] EL1 Fault detected! Advancing ELR_EL1 past faulting instruction...\r\n");
            ctx->elr_el1 += 4; // Skip faulting 4-byte instruction
            return;            // Return to vector handler -> eret resumes shell execution
        }

        default:
        {
            print_str("\r\n[PANIC] Unhandled Synchronous Exception! EC=");
            print_hex64(ec);
            print_str(" ESR_EL1=");
            print_hex64(esr);
            print_str(" FAR_EL1=");
            print_hex64(far);
            print_str("\r\n");

            dump_registers(ctx);

            if ((ctx->spsr_el1 & 0x0F) == 0) {
                print_str("[TRAP] Terminating faulting EL0 process.\r\n");
                return_to_kernel(-1);
            }

            while (true) {
                HAL::cpu.halt();
            }
        }
    }
}

extern "C" void aarch64_handle_invalid_exception(HAL::CpuContext* ctx, uint64_t type) {
    const char* type_names[] = {
        "Unknown", "Current EL SP0 IRQ", "Current EL SP0 FIQ", "Current EL SP0 SError",
        "Unknown", "Current EL SPx IRQ", "Current EL SPx FIQ", "Current EL SPx SError",
        "Unknown", "Lower EL AArch64 IRQ", "Lower EL AArch64 FIQ", "Lower EL AArch64 SError",
        "Lower EL AArch32 Sync", "Lower EL AArch32 IRQ", "Lower EL AArch32 FIQ", "Lower EL AArch32 SError"
    };

    print_str("\r\n[PANIC] Invalid/Async Trap Captured: ");
    if (type < 16) {
        print_str(type_names[type]);
    } else {
        print_hex64(type);
    }
    print_str("\r\n");

    dump_registers(ctx);

    if ((ctx->spsr_el1 & 0x0F) == 0) {
        print_str("[TRAP] Terminating faulting EL0 process.\r\n");
        return_to_kernel(-3);
    }

    while (true) {
        HAL::cpu.halt();
    }
}