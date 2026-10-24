#include "hal/cpu.hpp"

extern "C" int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp);

namespace {
class AArch64CpuControl : public HAL::CpuControl {
  public:
    constexpr AArch64CpuControl() = default;

    void enable_interrupts() override { asm volatile("msr daifclr, #2" ::: "memory"); }
    void disable_interrupts() override { asm volatile("msr daifset, #2" ::: "memory"); }

    bool interrupts_enabled() const override {
        uint64_t daif;
        asm volatile("mrs %0, daif" : "=r"(daif));
        return (daif & (1 << 7)) == 0;
    }

    uint8_t current_el() const override {
        uint64_t elvl;
        asm volatile("mrs %0, CurrentEL" : "=r"(elvl));
        return static_cast<uint8_t>((elvl >> 2) & 0x3);
    }

    void halt() override { asm volatile("wfi"); }

    void switch_context(HAL::CpuContext** old_ctx, HAL::CpuContext* new_ctx) override {
        (void)old_ctx;
        (void)new_ctx;
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
                     "isb" ::: "memory");
    }

    int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp) noexcept override {
        return ::enter_user_mode(entry_point, user_sp);
    }
};

constinit AArch64CpuControl aarch64_cpu{};
} // namespace

namespace HAL {
CpuControl& get_cpu() noexcept { return aarch64_cpu; }

PlatformInfo get_platform_info() noexcept {
    return PlatformInfo{
        .ram_base = 0x40000000U,
        .ram_size = 256UL * 1024UL * 1024UL,
        .page_size = 4096U
    };
}
} // namespace HAL