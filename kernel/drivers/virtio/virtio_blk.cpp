#include "drivers/virtio/virtio_blk.hpp"

#include "drivers/irq/gicv2.hpp"
#include "memory_config.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"
#include "process/process.hpp"

#include <cerrno>
#include <cstring>

#ifndef EIO
#define EIO 5
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOTSUP
#define ENOTSUP 95
#endif

namespace Kernel::Drivers::Virtio {

namespace {

class ScopedIrqDisable final {
  public:
    ScopedIrqDisable() noexcept {
#if defined(__aarch64__) && !defined(HOST_TEST)
        asm volatile("mrs %0, daif\n\t"
                     "msr daifset, #2"
                     : "=r"(m_saved_daif)
                     :
                     : "memory");
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

    ~ScopedIrqDisable() noexcept {
#if defined(__aarch64__) && !defined(HOST_TEST)
        asm volatile("msr daif, %0" : : "r"(m_saved_daif) : "memory");
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

    ScopedIrqDisable(const ScopedIrqDisable&) = delete;
    ScopedIrqDisable& operator=(const ScopedIrqDisable&) = delete;
    ScopedIrqDisable(ScopedIrqDisable&&) = delete;
    ScopedIrqDisable& operator=(ScopedIrqDisable&&) = delete;

  private:
#if defined(__aarch64__) && !defined(HOST_TEST)
    uint64_t m_saved_daif{0ULL};
#endif
};

uint16_t select_queue_size(uint32_t device_max, uint16_t preferred) noexcept {
    if (device_max == 0U || preferred == 0U) {
        return 0U;
    }
    uint32_t capped = device_max;
    if (capped > VIRTQ_MAX_SIZE) {
        capped = VIRTQ_MAX_SIZE;
    }
    if (capped > preferred) {
        capped = preferred;
    }
    uint16_t size = 1U;
    while ((static_cast<uint32_t>(size) << 1U) <= capped) {
        size = static_cast<uint16_t>(size << 1U);
    }
    return size;
}

} // namespace

VirtioBlkDevice::~VirtioBlkDevice() noexcept {
    shutdown();
}

bool VirtioBlkDevice::init(uintptr_t base_paddr, uint32_t spi_irq, size_t region_size,
                           uint16_t preferred_queue_size) noexcept {
    if (m_initialized) {
        shutdown();
    }

    if (!m_transport.init(base_paddr, region_size, VIRTIO_F_VERSION_1)) {
        return false;
    }

    if (m_transport.device_id() != VIRTIO_DEV_ID_BLK) {
        m_transport.set_failed();
        return false;
    }

    const uint32_t dev_max_q = m_transport.max_queue_size(0U);
    const uint16_t q_size = select_queue_size(dev_max_q, preferred_queue_size);
    if (q_size == 0U || !m_queue.init(q_size)) {
        m_transport.set_failed();
        return false;
    }

    if (!m_transport.setup_queue(0U, m_queue)) {
        m_queue.destroy();
        m_transport.set_failed();
        return false;
    }

    m_dma_paddr = Kernel::MM::alloc_frame();
    if (m_dma_paddr == 0U) {
        m_queue.destroy();
        m_transport.set_failed();
        return false;
    }
    std::memset(reinterpret_cast<void*>(Kernel::MM::phys_to_virt(m_dma_paddr)), 0,
                Arch::AArch64::PAGE_SIZE);

    // Read 64-bit sector capacity from Virtio-MMIO device config space (0x100)
    const uint32_t cap_lo = m_transport.read_config32(0U);
    const uint32_t cap_hi = m_transport.read_config32(4U);
    m_capacity_sectors = (static_cast<uint64_t>(cap_hi) << 32U) | static_cast<uint64_t>(cap_lo);
    if (m_capacity_sectors == 0ULL) {
        Kernel::MM::free_frame(m_dma_paddr);
        m_dma_paddr = 0U;
        m_queue.destroy();
        m_transport.set_failed();
        return false;
    }

    for (size_t i = 0U; i < VIRTQ_MAX_SIZE; ++i) {
        m_in_flight[i] = nullptr;
    }

    m_spi_irq = spi_irq;
    if (m_spi_irq >= IRQ::MIN_SPI_IRQ) {
        if (!IRQ::GicV2::register_spi_handler(m_spi_irq, &VirtioBlkDevice::irq_trampoline, this,
                                              IRQ::IrqTriggerMode::LevelHigh)) {
            Kernel::MM::free_frame(m_dma_paddr);
            m_dma_paddr = 0U;
            m_queue.destroy();
            m_transport.set_failed();
            return false;
        }
        IRQ::GicV2::enable_irq(m_spi_irq);
        m_irq_registered = true;
    }

    m_dma_busy = false;
    m_transport.set_driver_ok();
    m_initialized = true;
    return true;
}

void VirtioBlkDevice::shutdown() noexcept {
    if (m_irq_registered && m_spi_irq >= IRQ::MIN_SPI_IRQ) {
        IRQ::GicV2::disable_irq(m_spi_irq);
        IRQ::GicV2::unregister_spi_handler(m_spi_irq);
        m_irq_registered = false;
    }

    if (m_transport.is_valid()) {
        m_transport.reset();
    }

    if (m_queue.is_initialized()) {
        m_queue.destroy();
    }

    if (m_dma_paddr != 0U) {
        Kernel::MM::free_frame(m_dma_paddr);
        m_dma_paddr = 0U;
    }

    for (size_t i = 0U; i < VIRTQ_MAX_SIZE; ++i) {
        m_in_flight[i] = nullptr;
    }

    m_capacity_sectors = 0ULL;
    m_spi_irq = 0U;
    m_dma_busy = false;
    m_initialized = false;
}

int VirtioBlkDevice::read_sectors(uint64_t lba, size_t count, void* dst) noexcept {
    if (!m_initialized || count == 0U || dst == nullptr || lba > m_capacity_sectors ||
        count > (m_capacity_sectors - lba)) {
        return -EINVAL;
    }

    auto* out_bytes = static_cast<uint8_t*>(dst);
    size_t remaining = count;
    uint64_t cur_lba = lba;

    while (remaining > 0U) {
        const size_t chunk =
            (remaining > MAX_SECTORS_PER_CHUNK) ? MAX_SECTORS_PER_CHUNK : remaining;
        const int rc = submit_chunk(VIRTIO_BLK_T_IN, cur_lba, out_bytes, chunk);
        if (rc != 0) {
            return rc;
        }
        out_bytes += chunk * sector_size();
        cur_lba += chunk;
        remaining -= chunk;
    }
    return 0;
}

int VirtioBlkDevice::write_sectors(uint64_t lba, size_t count, const void* src) noexcept {
    if (!m_initialized || count == 0U || src == nullptr || lba > m_capacity_sectors ||
        count > (m_capacity_sectors - lba)) {
        return -EINVAL;
    }

    const auto* in_bytes = static_cast<const uint8_t*>(src);
    size_t remaining = count;
    uint64_t cur_lba = lba;

    while (remaining > 0U) {
        const size_t chunk =
            (remaining > MAX_SECTORS_PER_CHUNK) ? MAX_SECTORS_PER_CHUNK : remaining;
        const int rc =
            submit_chunk(VIRTIO_BLK_T_OUT, cur_lba, const_cast<uint8_t*>(in_bytes), chunk);
        if (rc != 0) {
            return rc;
        }
        in_bytes += chunk * sector_size();
        cur_lba += chunk;
        remaining -= chunk;
    }
    return 0;
}

int VirtioBlkDevice::flush() noexcept {
    if (!m_initialized) {
        return -EINVAL;
    }
    return submit_chunk(VIRTIO_BLK_T_FLUSH, 0ULL, nullptr, 0U);
}

int VirtioBlkDevice::submit_chunk(uint32_t type, uint64_t lba, void* buf,
                                  size_t sector_count) noexcept {
    VirtioBlkRequest req{};
    req.completed.store(false, std::memory_order_relaxed);
    req.status = VIRTIO_BLK_S_UNSET;
    if (auto* const proc = Process::current_process(); proc != nullptr) {
        req.waiter_pid = proc->pid;
    }

    ScopedIrqDisable irq_guard{};

    while (m_dma_busy) {
        m_dma_waiters.sleep();
    }
    m_dma_busy = true;

    const uintptr_t dma_kva = Kernel::MM::phys_to_virt(m_dma_paddr);
    auto* const hdr = reinterpret_cast<VirtioBlkReqHdr*>(dma_kva + DMA_HDR_OFFSET);
    auto* const status_ptr = reinterpret_cast<volatile uint8_t*>(dma_kva + DMA_STATUS_OFFSET);
    auto* const data_kva = reinterpret_cast<uint8_t*>(dma_kva + DMA_DATA_OFFSET);

    hdr->type = type;
    hdr->reserved = 0U;
    hdr->sector = lba;
    *status_ptr = VIRTIO_BLK_S_UNSET;

    const size_t byte_len = sector_count * sector_size();
    if (type == VIRTIO_BLK_T_OUT && byte_len > 0U) {
        std::memcpy(data_kva, buf, byte_len);
    }

    VirtqBuffer buffers[3]{};
    size_t desc_count = 0U;

    if (type == VIRTIO_BLK_T_FLUSH) {
        buffers[0] = VirtqBuffer{
            .paddr = m_dma_paddr + DMA_HDR_OFFSET,
            .len = sizeof(VirtioBlkReqHdr),
            .device_writable = false,
        };
        buffers[1] = VirtqBuffer{
            .paddr = m_dma_paddr + DMA_STATUS_OFFSET,
            .len = sizeof(uint8_t),
            .device_writable = true,
        };
        desc_count = 2U;
    } else {
        // 3-descriptor Virtio block request chain using physical RAM addresses:
        //   [0] Device-readable 16-byte VirtioBlkReqHdr
        //   [1] Data buffer (VRING_DESC_F_WRITE set for reads, clear for writes)
        //   [2] Device-writable 1-byte status field
        buffers[0] = VirtqBuffer{
            .paddr = m_dma_paddr + DMA_HDR_OFFSET,
            .len = sizeof(VirtioBlkReqHdr),
            .device_writable = false,
        };
        buffers[1] = VirtqBuffer{
            .paddr = m_dma_paddr + DMA_DATA_OFFSET,
            .len = static_cast<uint32_t>(byte_len),
            .device_writable = (type == VIRTIO_BLK_T_IN),
        };
        buffers[2] = VirtqBuffer{
            .paddr = m_dma_paddr + DMA_STATUS_OFFSET,
            .len = sizeof(uint8_t),
            .device_writable = true,
        };
        desc_count = 3U;
    }

    uint16_t head_idx = VRING_INVALID_DESC;
    if (!m_queue.submit_chain(buffers, desc_count, &head_idx) || head_idx >= VIRTQ_MAX_SIZE) {
        m_dma_busy = false;
        m_dma_waiters.wake_one();
        return -EBUSY;
    }

    m_in_flight[head_idx] = &req;

    virtio_wmb();
    m_transport.notify_queue(0U);

#if defined(HOST_TEST)
    if (m_host_notify_cb != nullptr) {
        m_host_notify_cb(*this, m_host_notify_ctx);
    }
#endif

    while (!req.completed.load(std::memory_order_acquire)) {
        m_sleep_count.fetch_add(1ULL, std::memory_order_relaxed);
        if (Process::current_process() != nullptr) {
            req.wait_queue.sleep();
        } else {
#if defined(__aarch64__) && !defined(HOST_TEST)
            // Early boot before scheduler starts: briefly unmask IRQs and wait for SPI.
            asm volatile("msr daifclr, #2\n\t"
                         "wfi\n\t"
                         "msr daifset, #2"
                         :
                         :
                         : "memory");
#endif
        }
    }

    const uint8_t final_status = *status_ptr;
    if (type == VIRTIO_BLK_T_IN && final_status == VIRTIO_BLK_S_OK && byte_len > 0U) {
        std::memcpy(buf, data_kva, byte_len);
    }

    m_dma_busy = false;
    m_dma_waiters.wake_one();

    return translate_status(final_status);
}

void VirtioBlkDevice::handle_irq() noexcept {
    const uint32_t isr = m_transport.read_interrupt_status();
    if (isr == 0U) {
        return;
    }

    m_transport.ack_interrupt(isr);

    if ((isr & VIRTIO_MMIO_INT_VRING) == 0U) {
        return;
    }

    virtio_rmb();

    uint16_t head_idx = VRING_INVALID_DESC;
    uint32_t written_len = 0U;
    while (m_queue.pop_used(head_idx, written_len, /*auto_free=*/true)) {
        if (head_idx < VIRTQ_MAX_SIZE && m_in_flight[head_idx] != nullptr) {
            VirtioBlkRequest* const req = m_in_flight[head_idx];
            m_in_flight[head_idx] = nullptr;

            m_irq_completions.fetch_add(1ULL, std::memory_order_relaxed);
            m_last_woken_pid.store(req->waiter_pid, std::memory_order_relaxed);

            req->completed.store(true, std::memory_order_release);
            req->wait_queue.wake_one();
        }
    }
}

void VirtioBlkDevice::irq_trampoline(uint32_t irq, void* context) noexcept {
    (void)irq;
    auto* const dev = static_cast<VirtioBlkDevice*>(context);
    if (dev != nullptr) {
        dev->handle_irq();
    }
}

int VirtioBlkDevice::translate_status(uint8_t status) noexcept {
    switch (status) {
    case VIRTIO_BLK_S_OK:
        return 0;
    case VIRTIO_BLK_S_IOERR:
        return -EIO;
    case VIRTIO_BLK_S_UNSUPP:
        return -ENOTSUP;
    default:
        return -EIO;
    }
}

} // namespace Kernel::Drivers::Virtio