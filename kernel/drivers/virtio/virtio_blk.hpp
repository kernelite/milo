#pragma once

#include "drivers/block/block_device.hpp"
#include "drivers/virtio/SplitVirtqueue.hpp"
#include "drivers/virtio/VirtioMmioTransport.hpp"
#include "process/wait_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Kernel::Drivers::Virtio {

// Virtio block request types (Virtio Spec v1.2, Section 5.2.6)
constexpr uint32_t VIRTIO_BLK_T_IN = 0U;
constexpr uint32_t VIRTIO_BLK_T_OUT = 1U;
constexpr uint32_t VIRTIO_BLK_T_FLUSH = 4U;

// Virtio block status bytes
constexpr uint8_t VIRTIO_BLK_S_OK = 0U;
constexpr uint8_t VIRTIO_BLK_S_IOERR = 1U;
constexpr uint8_t VIRTIO_BLK_S_UNSUPP = 2U;
constexpr uint8_t VIRTIO_BLK_S_UNSET = 0xFFU;

// Virtio-MMIO device configuration offset (0x100)
constexpr size_t VIRTIO_MMIO_CONFIG_BASE = 0x100U;

struct [[gnu::packed]] VirtioBlkReqHdr {
    uint32_t type{0U};
    uint32_t reserved{0U};
    uint64_t sector{0ULL};
};
static_assert(sizeof(VirtioBlkReqHdr) == 16U, "VirtioBlkReqHdr must be 16 bytes");

struct [[gnu::packed]] VirtioBlkConfig {
    uint64_t capacity{0ULL}; // Total 512-byte sectors
};

struct VirtioBlkRequest {
    std::atomic<bool> completed{false};
    uint8_t status{VIRTIO_BLK_S_UNSET};
    uint32_t waiter_pid{0U};
    Process::WaitQueue wait_queue;
};

class VirtioBlkDevice final : public Block::BlockDevice {
  public:
    constexpr VirtioBlkDevice() noexcept = default;
    ~VirtioBlkDevice() noexcept override;

    VirtioBlkDevice(const VirtioBlkDevice&) = delete;
    VirtioBlkDevice& operator=(const VirtioBlkDevice&) = delete;
    VirtioBlkDevice(VirtioBlkDevice&&) = delete;
    VirtioBlkDevice& operator=(VirtioBlkDevice&&) = delete;

    [[nodiscard]] bool init(uintptr_t base_paddr, uint32_t spi_irq = 0U,
                            size_t region_size = 0x200U,
                            uint16_t preferred_queue_size = 128U) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] size_t sector_size() const noexcept override {
        return Block::DEFAULT_SECTOR_SIZE;
    }
    [[nodiscard]] uint64_t sector_count() const noexcept override { return m_capacity_sectors; }

    [[nodiscard]] int read_sectors(uint64_t lba, size_t count, void* dst) noexcept override;
    [[nodiscard]] int write_sectors(uint64_t lba, size_t count, const void* src) noexcept override;
    [[nodiscard]] int flush() noexcept;

    void handle_irq() noexcept;
    static void irq_trampoline(uint32_t irq, void* context) noexcept;

    [[nodiscard]] bool is_initialized() const noexcept { return m_initialized; }
    [[nodiscard]] uint32_t spi_irq() const noexcept { return m_spi_irq; }
    [[nodiscard]] VirtioMmioTransport& transport() noexcept { return m_transport; }
    [[nodiscard]] SplitVirtqueue& queue() noexcept { return m_queue; }
    [[nodiscard]] uint64_t irq_completions() const noexcept override {
        return m_irq_completions.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t sleep_count() const noexcept override {
        return m_sleep_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint32_t last_woken_pid() const noexcept override {
        return m_last_woken_pid.load(std::memory_order_relaxed);
    }

#if defined(HOST_TEST)
    using HostNotifyCallback = void (*)(VirtioBlkDevice& dev, void* ctx) noexcept;
    void set_host_notify_callback(HostNotifyCallback cb, void* ctx) noexcept {
        m_host_notify_cb = cb;
        m_host_notify_ctx = ctx;
    }
#endif

  private:
    // Layout inside the 4 KiB PFA-allocated DMA page (m_dma_paddr):
    //   0x000..0x00F: VirtioBlkReqHdr (16 bytes)
    //   0x010       : status byte (1 byte)
    //   0x040..0xE3F: 7-sector bounce buffer (3584 bytes, cacheline-aligned)
    static constexpr size_t DMA_HDR_OFFSET = 0x000U;
    static constexpr size_t DMA_STATUS_OFFSET = 0x010U;
    static constexpr size_t DMA_DATA_OFFSET = 0x040U;
    static constexpr size_t MAX_SECTORS_PER_CHUNK = 7U;

    [[nodiscard]] int submit_chunk(uint32_t type, uint64_t lba, void* buf,
                                   size_t sector_count) noexcept;
    [[nodiscard]] static int translate_status(uint8_t status) noexcept;

    VirtioMmioTransport m_transport;
    SplitVirtqueue m_queue;
    uintptr_t m_dma_paddr{0U};
    uint64_t m_capacity_sectors{0ULL};
    uint32_t m_spi_irq{0U};
    bool m_irq_registered{false};
    bool m_initialized{false};
    bool m_dma_busy{false};
    Process::WaitQueue m_dma_waiters;

    VirtioBlkRequest* m_in_flight[VIRTQ_MAX_SIZE]{};

    std::atomic<uint64_t> m_irq_completions{0ULL};
    std::atomic<uint64_t> m_sleep_count{0ULL};
    std::atomic<uint32_t> m_last_woken_pid{0U};

#if defined(HOST_TEST)
    HostNotifyCallback m_host_notify_cb{nullptr};
    void* m_host_notify_ctx{nullptr};
#endif
};

} // namespace Kernel::Drivers::Virtio