#pragma once

#include <cstddef>
#include <cstdint>

namespace drivers::block {

constexpr size_t kDefaultSectorSize = 512;

class BlockDevice {
  public:
    virtual ~BlockDevice() = default;

    [[nodiscard]] virtual size_t sector_size() const = 0;
    [[nodiscard]] virtual uint64_t sector_count() const = 0;

    // Returns 0 on success, or negative errno (e.g., -EIO, -EINVAL, -ENOTSUP) on failure.
    virtual int read_sectors(uint64_t lba, size_t count, void* dst) = 0;
    virtual int write_sectors(uint64_t lba, size_t count, const void* src) = 0;
};

} // namespace drivers::block