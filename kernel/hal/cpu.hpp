#pragma once
#include <cstddef>
#include <cstdint>

namespace HAL {

struct CpuContext {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
};

struct PlatformInfo {
    uintptr_t ram_base;
    size_t ram_size;
    size_t page_size;
};

class CpuControl {
  public:
    constexpr CpuControl() = default;
    virtual ~CpuControl() = default;

    virtual void enable_interrupts() = 0;
    virtual void disable_interrupts() = 0;
    virtual bool interrupts_enabled() const = 0;

    virtual uint8_t current_el() const = 0;
    virtual void halt() = 0;

    virtual void switch_context(CpuContext** old_ctx, CpuContext* new_ctx) = 0;

    // --- Cache Operations ---
    virtual void flush_dcache_range(uintptr_t virt_addr, size_t size) noexcept = 0;
    virtual void invalidate_icache() noexcept = 0;

    // --- Execution Context Transitions ---
    virtual int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp) noexcept = 0;
};

CpuControl& get_cpu() noexcept;
PlatformInfo get_platform_info() noexcept;

} // namespace HAL