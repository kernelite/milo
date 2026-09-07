#include "hal/mmu.hpp"

namespace {
class AArch64MemoryControl : public HAL::MemoryControl {
public:
    constexpr AArch64MemoryControl() = default;

    bool map(uintptr_t, uintptr_t, HAL::PageFlags) override { return true; }
    bool unmap(uintptr_t) override { return true; }
    void activate() override {}

    HAL::MmuStatus status() const override {
        uint64_t sctlr;
        asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

        return HAL::MmuStatus{
            .mmu_enabled     = (sctlr & (1ULL << 0)) != 0,
            .dcache_enabled  = (sctlr & (1ULL << 2)) != 0,
            .icache_enabled  = (sctlr & (1ULL << 12)) != 0,
            .raw_control_reg = sctlr
        };
    }
};

constinit AArch64MemoryControl aarch64_mmu{};
}

namespace HAL {
    MemoryControl& mmu = aarch64_mmu;
}