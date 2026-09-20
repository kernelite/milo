#pragma once
#include <cstddef>
#include <cstdint>

namespace HAL {

enum class PageFlags : uint32_t {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    Execute = 1 << 2,
    User = 1 << 3,
    Device = 1 << 4,
    Cacheable = 1 << 5
};

struct MmuStatus {
    bool mmu_enabled;
    bool dcache_enabled;
    bool icache_enabled;
    uint64_t raw_control_reg;
};

class MemoryControl {
  public:
    constexpr MemoryControl() = default;
    virtual ~MemoryControl() = default;

    virtual bool map(uintptr_t virt_addr, uintptr_t phys_addr, PageFlags flags) = 0;
    virtual bool unmap(uintptr_t virt_addr) = 0;
    virtual void activate() = 0;
    virtual MmuStatus status() const noexcept = 0;
};

MemoryControl& get_mmu() noexcept;

} // namespace HAL