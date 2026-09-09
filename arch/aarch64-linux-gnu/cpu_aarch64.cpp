#include "hal/cpu.hpp"

namespace {
class AArch64CpuControl : public HAL::CpuControl {
  public:
    constexpr AArch64CpuControl() = default; // ADD THIS

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
};

constinit AArch64CpuControl aarch64_cpu{}; // Compile-time static initialization
} // namespace

namespace HAL {
CpuControl& get_cpu() noexcept {
    return aarch64_cpu;
}
} // namespace HAL