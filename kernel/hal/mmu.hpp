#pragma once
#include <cstddef>
#include <cstdint>

namespace HAL {

enum class PageFlags : uint32_t {
    None       = 0,
    Read       = 1 << 0,
    Write      = 1 << 1,
    Execute    = 1 << 2,
    User       = 1 << 3,
    Device     = 1 << 4, // Non-cacheable MMIO
    Cacheable  = 1 << 5  // Normal Cacheable RAM
};

inline PageFlags operator|(PageFlags a, PageFlags b) {
    return static_cast<PageFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

class PageTable {
public:
    virtual ~PageTable() = default;

    virtual bool map(uintptr_t virt_addr, uintptr_t phys_addr, PageFlags flags) = 0;
    virtual bool unmap(uintptr_t virt_addr) = 0;
    virtual void activate() = 0;
};

}