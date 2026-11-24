#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Drivers::Block {

constexpr size_t DEFAULT_SECTOR_SIZE = 512U;

class BlockDevice {
  public:
    virtual ~BlockDevice() noexcept = default;

    [[nodiscard]] virtual size_t sector_size() const noexcept = 0;
    [[nodiscard]] virtual uint64_t sector_count() const noexcept = 0;

    // Reads `count` sectors starting at `lba` into `dst`.
    // Returns 0 on success, or negative errno (-EINVAL, -EIO, -ENOTSUP) on failure.
    [[nodiscard]] virtual int read_sectors(uint64_t lba, size_t count, void* dst) noexcept = 0;

    // Writes `count` sectors starting at `lba` from `src`.
    // Returns 0 on success, or negative errno (-EINVAL, -EIO, -ENOTSUP) on failure.
    [[nodiscard]] virtual int write_sectors(uint64_t lba, size_t count,
                                            const void* src) noexcept = 0;

    // Optional async IRQ & WaitQueue instrumentation hooks.
    [[nodiscard]] virtual uint64_t irq_completions() const noexcept { return 0ULL; }
    [[nodiscard]] virtual uint64_t sleep_count() const noexcept { return 0ULL; }
    [[nodiscard]] virtual uint32_t last_woken_pid() const noexcept { return 0U; }
};

} // namespace Kernel::Drivers::Block