#include "hal/cpu.hpp"

#include "cpu_context.hpp"
#include "memory_config.hpp"

extern "C" {
void cpu_switch_context(HAL::KernelContext* prev, const HAL::KernelContext* next) noexcept;
void user_trap_exit() noexcept;
int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp);
int64_t enter_user_context(const HAL::CpuContext* ctx);
[[noreturn]] void return_to_kernel(int64_t exit_code);
void* memcpy(void* dest, const void* src, size_t count) noexcept;
}

namespace {

constexpr uint16_t ELF_MACHINE_AARCH64 = 183U;
constexpr uintptr_t AARCH64_HIGHER_HALF_OFFSET = 0xFFFF800000000000ULL;
constexpr uintptr_t AARCH64_USER_VA_LIMIT = 0x0000000040000000ULL;
constexpr uintptr_t AARCH64_USER_STACK_TOP = 0x000000003FFF0000ULL;
constexpr uintptr_t AARCH64_USER_BRK_BASE = 0x0000000010000000ULL;
constexpr uintptr_t AARCH64_USER_MMAP_BASE = 0x000000003FFE0000ULL;

class AArch64CpuControl : public HAL::CpuControl {
  public:
    constexpr AArch64CpuControl() = default;

    void enable_interrupts() override { asm volatile("msr daifclr, #2" ::: "memory"); }
    void disable_interrupts() override { asm volatile("msr daifset, #2" ::: "memory"); }

    bool interrupts_enabled() const override {
        uint64_t daif;
        asm volatile("mrs %0, daif" : "=r"(daif));
        return (daif & (1U << 7U)) == 0U;
    }

    uint8_t current_el() const override {
        uint64_t elvl;
        asm volatile("mrs %0, CurrentEL" : "=r"(elvl));
        return static_cast<uint8_t>((elvl >> 2U) & 0x3U);
    }

    void halt() override { asm volatile("wfi"); }

    void switch_context(HAL::CpuContext** old_ctx, HAL::CpuContext* new_ctx) override {
        (void)old_ctx;
        (void)new_ctx;
    }

    void switch_kernel_context(HAL::KernelContext* prev,
                               const HAL::KernelContext* next) noexcept override {
        if (next != nullptr) {
            cpu_switch_context(prev, next);
        }
    }

    void init_user_kstack(HAL::KernelContext& kctx, uintptr_t kstack_top,
                          const HAL::CpuContext& user_ctx) const noexcept override {
        const uintptr_t frame_addr = kstack_top - sizeof(HAL::CpuContext);
        auto* const frame_ptr = reinterpret_cast<HAL::CpuContext*>(frame_addr);
        memcpy(frame_ptr, &user_ctx, sizeof(HAL::CpuContext));

        auto* const arch_kctx = reinterpret_cast<AArch64KernelContext*>(&kctx);
        arch_kctx->x19 = 0ULL;
        arch_kctx->x20 = 0ULL;
        arch_kctx->x21 = 0ULL;
        arch_kctx->x22 = 0ULL;
        arch_kctx->x23 = 0ULL;
        arch_kctx->x24 = 0ULL;
        arch_kctx->x25 = 0ULL;
        arch_kctx->x26 = 0ULL;
        arch_kctx->x27 = 0ULL;
        arch_kctx->x28 = 0ULL;
        arch_kctx->x29 = 0ULL;
        arch_kctx->x30 = reinterpret_cast<uintptr_t>(&user_trap_exit);
        arch_kctx->sp = frame_addr;
        arch_kctx->_pad = 0ULL;
    }

    void flush_dcache_range(uintptr_t addr, size_t size) noexcept override {
        constexpr uintptr_t line_size = 64U;
        const uintptr_t end = addr + size;
        addr &= ~(line_size - 1U);
        for (; addr < end; addr += line_size) {
            asm volatile("dc civac, %0" ::"r"(addr) : "memory");
        }
        asm volatile("dsb sy" ::: "memory");
    }

    void invalidate_icache() noexcept override {
        asm volatile("dsb ish\n\t"
                     "ic ialluis\n\t"
                     "dsb ish\n\t"
                     "isb" ::
                         : "memory");
    }

    void init_user_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t user_sp,
                           uint64_t arg0, uint64_t arg1) const noexcept override {
        auto* const arch_ctx = reinterpret_cast<AArch64CpuContext*>(&ctx);
        for (size_t idx = 0U; idx < 31U; ++idx) {
            arch_ctx->x[idx] = 0ULL;
        }
        arch_ctx->x[0] = arg0;
        arch_ctx->x[1] = arg1;
        arch_ctx->sp_el0 = user_sp;
        arch_ctx->elr_el1 = entry_point;
        arch_ctx->spsr_el1 = 0x00ULL; // EL0t mode with DAIF unmasked
        arch_ctx->tpidr_el0 = 0ULL;
        arch_ctx->_pad = 0ULL;
    }

    void set_user_tls_pointer(HAL::CpuContext& ctx, uintptr_t tls) const noexcept override {
        auto* const arch_ctx = reinterpret_cast<AArch64CpuContext*>(&ctx);
        arch_ctx->tpidr_el0 = tls;
    }

    uintptr_t get_user_tls_pointer(const HAL::CpuContext& ctx) const noexcept override {
        const auto* const arch_ctx = reinterpret_cast<const AArch64CpuContext*>(&ctx);
        return static_cast<uintptr_t>(arch_ctx->tpidr_el0);
    }

    int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp) noexcept override {
        return ::enter_user_mode(entry_point, user_sp);
    }

    int64_t enter_user_context(const HAL::CpuContext& ctx) noexcept override {
        return ::enter_user_context(&ctx);
    }

    [[noreturn]] void return_to_kernel(int64_t exit_code) noexcept override {
        ::return_to_kernel(exit_code);
    }

    uint64_t get_syscall_number(const HAL::CpuContext& ctx) const noexcept override {
        const auto* const arch_ctx = reinterpret_cast<const AArch64CpuContext*>(&ctx);
        return arch_ctx->x[8];
    }

    uint64_t get_syscall_arg(const HAL::CpuContext& ctx, size_t index) const noexcept override {
        if (index >= 6U) {
            return 0ULL;
        }
        const auto* const arch_ctx = reinterpret_cast<const AArch64CpuContext*>(&ctx);
        return arch_ctx->x[index];
    }

    void set_syscall_return(HAL::CpuContext& ctx, int64_t value) const noexcept override {
        auto* const arch_ctx = reinterpret_cast<AArch64CpuContext*>(&ctx);
        arch_ctx->x[0] = static_cast<uint64_t>(value);
    }

    void set_user_stack_pointer(HAL::CpuContext& ctx, uintptr_t user_sp) const noexcept override {
        auto* const arch_ctx = reinterpret_cast<AArch64CpuContext*>(&ctx);
        arch_ctx->sp_el0 = user_sp;
    }
};

constinit AArch64CpuControl aarch64_cpu{};
} // namespace

namespace HAL {
CpuControl& get_cpu() noexcept {
    return aarch64_cpu;
}

PlatformInfo get_platform_info() noexcept {
    return PlatformInfo{
        .ram_base = Arch::RAM_BASE,
        .ram_size = 256UL * 1024UL * 1024UL,
        .max_ram_size = Arch::MAX_RAM_SIZE,
        .page_size = Arch::PAGE_SIZE,
        .higher_half_offset = AARCH64_HIGHER_HALF_OFFSET,
        .user_va_limit = AARCH64_USER_VA_LIMIT,
        .user_stack_top = AARCH64_USER_STACK_TOP,
        .user_brk_base = AARCH64_USER_BRK_BASE,
        .user_mmap_base = AARCH64_USER_MMAP_BASE,
        .elf_machine = ELF_MACHINE_AARCH64,
    };
}
} // namespace HAL