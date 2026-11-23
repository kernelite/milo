#pragma once

#include "drivers/virtio/virtio_ring.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Drivers::Virtio {

constexpr uint32_t VIRTIO_MMIO_MAGIC = 0x74726976U; // "virt"
constexpr uint32_t VIRTIO_MMIO_VERSION_MODERN = 2U;

constexpr uint32_t VIRTIO_DEV_ID_INVALID = 0U;
constexpr uint32_t VIRTIO_DEV_ID_NET = 1U;
constexpr uint32_t VIRTIO_DEV_ID_BLK = 2U;
constexpr uint32_t VIRTIO_DEV_ID_CONSOLE = 3U;
constexpr uint32_t VIRTIO_DEV_ID_RNG = 4U;

constexpr uint32_t VIRTIO_STATUS_RESET = 0x00U;
constexpr uint32_t VIRTIO_STATUS_ACKNOWLEDGE = 0x01U;
constexpr uint32_t VIRTIO_STATUS_DRIVER = 0x02U;
constexpr uint32_t VIRTIO_STATUS_DRIVER_OK = 0x04U;
constexpr uint32_t VIRTIO_STATUS_FEATURES_OK = 0x08U;
constexpr uint32_t VIRTIO_STATUS_DEVICE_NEEDS_RESET = 0x40U;
constexpr uint32_t VIRTIO_STATUS_FAILED = 0x80U;

constexpr uint32_t VIRTIO_MMIO_INT_VRING = 1U << 0U;
constexpr uint32_t VIRTIO_MMIO_INT_CONFIG = 1U << 1U;

constexpr uint64_t VIRTIO_F_RING_INDIRECT_DESC = 1ULL << 28U;
constexpr uint64_t VIRTIO_F_RING_EVENT_IDX = 1ULL << 29U;
constexpr uint64_t VIRTIO_F_VERSION_1 = 1ULL << 32U;

class VirtioMmioTransport final {
  public:
    constexpr VirtioMmioTransport() noexcept = default;

    [[nodiscard]] bool probe(uintptr_t base_paddr, size_t region_size = 0x200U) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool
    negotiate_features(uint64_t driver_supported_features = VIRTIO_F_VERSION_1) noexcept;

    [[nodiscard]] uint32_t max_queue_size(uint32_t queue_idx) noexcept;
    [[nodiscard]] bool setup_queue(uint32_t queue_idx, const SplitVirtqueue& vq) noexcept;

    void set_driver_ok() noexcept;
    void set_failed() noexcept;

    [[nodiscard]] bool init(uintptr_t base_paddr, size_t region_size = 0x200U,
                            uint64_t driver_supported_features = VIRTIO_F_VERSION_1) noexcept;

    void notify_queue(uint32_t queue_idx) noexcept;
    [[nodiscard]] uint32_t read_interrupt_status() const noexcept;
    void ack_interrupt(uint32_t status_bits) noexcept;

    [[nodiscard]] uint8_t read_config8(size_t offset) const noexcept;
    [[nodiscard]] uint32_t read_config32(size_t offset) const noexcept;
    [[nodiscard]] uint64_t read_config64(size_t offset) const noexcept;

    [[nodiscard]] bool is_valid() const noexcept { return m_valid; }
    [[nodiscard]] uintptr_t base_paddr() const noexcept { return m_base_paddr; }
    [[nodiscard]] uintptr_t virt_base() const noexcept { return m_virt_base; }
    [[nodiscard]] uint32_t device_id() const noexcept { return m_device_id; }
    [[nodiscard]] uint32_t vendor_id() const noexcept { return m_vendor_id; }
    [[nodiscard]] uint64_t device_features() const noexcept { return m_device_features; }
    [[nodiscard]] uint64_t negotiated_features() const noexcept { return m_negotiated_features; }
    [[nodiscard]] uint32_t status() const noexcept;

  private:
    [[nodiscard]] uint32_t read_reg(size_t offset) const noexcept;
    void write_reg(size_t offset, uint32_t value) noexcept;

    uintptr_t m_base_paddr{0U};
    uintptr_t m_virt_base{0U};
    size_t m_region_size{0U};
    uint32_t m_device_id{VIRTIO_DEV_ID_INVALID};
    uint32_t m_vendor_id{0U};
    uint64_t m_device_features{0ULL};
    uint64_t m_negotiated_features{0ULL};
    bool m_valid{false};
};

} // namespace Kernel::Drivers::Virtio