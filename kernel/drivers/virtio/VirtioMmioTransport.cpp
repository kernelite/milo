#include "drivers/virtio/VirtioMmioTransport.hpp"

#include "hal/mmu.hpp"
#include "memory_config.hpp"
#include "mm/AddressSpace.hpp"

namespace Kernel::Drivers::Virtio {

namespace {

constexpr size_t REG_MAGIC_VALUE = 0x000U;
constexpr size_t REG_VERSION = 0x004U;
constexpr size_t REG_DEVICE_ID = 0x008U;
constexpr size_t REG_VENDOR_ID = 0x00CU;
constexpr size_t REG_DEVICE_FEATURES = 0x010U;
constexpr size_t REG_DEVICE_FEATURES_SEL = 0x014U;
constexpr size_t REG_DRIVER_FEATURES = 0x020U;
constexpr size_t REG_DRIVER_FEATURES_SEL = 0x024U;
constexpr size_t REG_QUEUE_SEL = 0x030U;
constexpr size_t REG_QUEUE_NUM_MAX = 0x034U;
constexpr size_t REG_QUEUE_NUM = 0x038U;
constexpr size_t REG_QUEUE_READY = 0x044U;
constexpr size_t REG_QUEUE_NOTIFY = 0x050U;
constexpr size_t REG_INTERRUPT_STATUS = 0x060U;
constexpr size_t REG_INTERRUPT_ACK = 0x064U;
constexpr size_t REG_STATUS = 0x070U;
constexpr size_t REG_QUEUE_DESC_LOW = 0x080U;
constexpr size_t REG_QUEUE_DESC_HIGH = 0x084U;
constexpr size_t REG_QUEUE_DRIVER_LOW = 0x090U;
constexpr size_t REG_QUEUE_DRIVER_HIGH = 0x094U;
constexpr size_t REG_QUEUE_DEVICE_LOW = 0x0A0U;
constexpr size_t REG_QUEUE_DEVICE_HIGH = 0x0A4U;
constexpr size_t REG_CONFIG_SPACE = 0x100U;

constexpr size_t RESET_POLL_LIMIT = 100000U;

bool map_mmio_region(uintptr_t base_paddr, size_t region_size) noexcept {
    if (base_paddr == 0U || region_size == 0U) {
        return false;
    }

    const uintptr_t page_mask = Arch::AArch64::PAGE_SIZE - 1U;
    const uintptr_t start_paddr = base_paddr & ~page_mask;
    const uintptr_t end_paddr = (base_paddr + region_size + page_mask) & ~page_mask;
    const HAL::PageFlags dev_rw =
        HAL::PageFlags::Read | HAL::PageFlags::Write | HAL::PageFlags::Device;

    for (uintptr_t paddr = start_paddr; paddr < end_paddr; paddr += Arch::AArch64::PAGE_SIZE) {
        const uintptr_t vaddr = Kernel::MM::phys_to_virt(paddr);
        if (!HAL::get_mmu().map(vaddr, paddr, dev_rw)) {
            return false;
        }
    }
    return true;
}

} // namespace

uint32_t VirtioMmioTransport::read_reg(size_t offset) const noexcept {
    auto* const reg = reinterpret_cast<volatile uint32_t*>(m_virt_base + offset);
    return *reg;
}

void VirtioMmioTransport::write_reg(size_t offset, uint32_t value) const noexcept {
    auto* const reg = reinterpret_cast<volatile uint32_t*>(m_virt_base + offset);
    *reg = value;
}

bool VirtioMmioTransport::probe(uintptr_t base_paddr, size_t region_size) noexcept {
    m_valid = false;
    m_device_id = VIRTIO_DEV_ID_INVALID;
    m_vendor_id = 0U;
    m_device_features = 0ULL;
    m_negotiated_features = 0ULL;

    const size_t effective_size = (region_size > 0U) ? region_size : 0x200U;
    if (!map_mmio_region(base_paddr, effective_size)) {
        return false;
    }

    m_base_paddr = base_paddr;
    m_virt_base = Kernel::MM::phys_to_virt(base_paddr);
    m_region_size = effective_size;

    const uint32_t magic = read_reg(REG_MAGIC_VALUE);
    if (magic != VIRTIO_MMIO_MAGIC) {
        return false;
    }

    const uint32_t version = read_reg(REG_VERSION);
    if (version != VIRTIO_MMIO_VERSION_MODERN) {
        return false;
    }

    const uint32_t dev_id = read_reg(REG_DEVICE_ID);
    if (dev_id == VIRTIO_DEV_ID_INVALID) {
        return false;
    }

    m_device_id = dev_id;
    m_vendor_id = read_reg(REG_VENDOR_ID);
    m_valid = true;
    return true;
}

void VirtioMmioTransport::reset() noexcept {
    if (m_virt_base == 0U) {
        return;
    }
    write_reg(REG_STATUS, VIRTIO_STATUS_RESET);
    virtio_mb();

    for (size_t attempt = 0U; attempt < RESET_POLL_LIMIT; ++attempt) {
        if (read_reg(REG_STATUS) == VIRTIO_STATUS_RESET) {
            break;
        }
    }
}

bool VirtioMmioTransport::negotiate_features(uint64_t driver_supported_features) noexcept {
    if (!m_valid) {
        return false;
    }

    // 1. Reset device
    reset();

    // 2. Acknowledge device
    write_reg(REG_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_mb();

    // 3. Signal driver presence
    write_reg(REG_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    virtio_mb();

    // 4. Read 64-bit device feature bitmap
    write_reg(REG_DEVICE_FEATURES_SEL, 0U);
    virtio_mb();
    const uint32_t features_low = read_reg(REG_DEVICE_FEATURES);

    write_reg(REG_DEVICE_FEATURES_SEL, 1U);
    virtio_mb();
    const uint32_t features_high = read_reg(REG_DEVICE_FEATURES);

    m_device_features = (static_cast<uint64_t>(features_high) << 32U) | features_low;
    if ((m_device_features & VIRTIO_F_VERSION_1) == 0ULL) {
        set_failed();
        return false;
    }

    // 5. Write negotiated feature subset (requiring VIRTIO_F_VERSION_1)
    m_negotiated_features = (m_device_features & driver_supported_features) | VIRTIO_F_VERSION_1;

    write_reg(REG_DRIVER_FEATURES_SEL, 0U);
    virtio_mb();
    write_reg(REG_DRIVER_FEATURES, static_cast<uint32_t>(m_negotiated_features & 0xFFFFFFFFULL));

    write_reg(REG_DRIVER_FEATURES_SEL, 1U);
    virtio_mb();
    write_reg(REG_DRIVER_FEATURES, static_cast<uint32_t>(m_negotiated_features >> 32U));
    virtio_mb();

    // 6. Set FEATURES_OK and verify the device accepted the feature set
    const uint32_t features_ok_status =
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    write_reg(REG_STATUS, features_ok_status);
    virtio_mb();

    const uint32_t verified_status = read_reg(REG_STATUS);
    if ((verified_status & VIRTIO_STATUS_FEATURES_OK) == 0U) {
        set_failed();
        return false;
    }

    return true;
}

uint32_t VirtioMmioTransport::max_queue_size(uint32_t queue_idx) noexcept {
    if (!m_valid) {
        return 0U;
    }
    write_reg(REG_QUEUE_SEL, queue_idx);
    virtio_mb();
    return read_reg(REG_QUEUE_NUM_MAX);
}

bool VirtioMmioTransport::setup_queue(uint32_t queue_idx, const SplitVirtqueue& vq) noexcept {
    if (!m_valid || !vq.is_initialized()) {
        return false;
    }

    write_reg(REG_QUEUE_SEL, queue_idx);
    virtio_mb();

    if (read_reg(REG_QUEUE_READY) != 0U) {
        return false;
    }

    const uint32_t max_size = read_reg(REG_QUEUE_NUM_MAX);
    if (max_size == 0U || vq.queue_size() > max_size) {
        return false;
    }

    write_reg(REG_QUEUE_NUM, vq.queue_size());

    const uint64_t desc_addr = static_cast<uint64_t>(vq.desc_paddr());
    const uint64_t avail_addr = static_cast<uint64_t>(vq.avail_paddr());
    const uint64_t used_addr = static_cast<uint64_t>(vq.used_paddr());

    write_reg(REG_QUEUE_DESC_LOW, static_cast<uint32_t>(desc_addr & 0xFFFFFFFFULL));
    write_reg(REG_QUEUE_DESC_HIGH, static_cast<uint32_t>(desc_addr >> 32U));

    write_reg(REG_QUEUE_DRIVER_LOW, static_cast<uint32_t>(avail_addr & 0xFFFFFFFFULL));
    write_reg(REG_QUEUE_DRIVER_HIGH, static_cast<uint32_t>(avail_addr >> 32U));

    write_reg(REG_QUEUE_DEVICE_LOW, static_cast<uint32_t>(used_addr & 0xFFFFFFFFULL));
    write_reg(REG_QUEUE_DEVICE_HIGH, static_cast<uint32_t>(used_addr >> 32U));

    virtio_mb();
    write_reg(REG_QUEUE_READY, 1U);
    virtio_mb();
    return read_reg(REG_QUEUE_READY) == 1U;
}

void VirtioMmioTransport::set_driver_ok() noexcept {
    if (!m_valid) {
        return;
    }
    const uint32_t current = read_reg(REG_STATUS);
    write_reg(REG_STATUS, current | VIRTIO_STATUS_DRIVER_OK);
    virtio_mb();
}

void VirtioMmioTransport::set_failed() noexcept {
    if (m_virt_base == 0U) {
        return;
    }
    const uint32_t current = read_reg(REG_STATUS);
    write_reg(REG_STATUS, current | VIRTIO_STATUS_FAILED);
    virtio_mb();
}

bool VirtioMmioTransport::init(uintptr_t base_paddr, size_t region_size,
                               uint64_t driver_supported_features) noexcept {
    if (!probe(base_paddr, region_size)) {
        return false;
    }
    if (!negotiate_features(driver_supported_features)) {
        return false;
    }
    set_driver_ok();
    return (status() & VIRTIO_STATUS_DRIVER_OK) != 0U;
}

void VirtioMmioTransport::notify_queue(uint32_t queue_idx) noexcept {
    if (!m_valid) {
        return;
    }
    virtio_mb();
    write_reg(REG_QUEUE_NOTIFY, queue_idx);
}

uint32_t VirtioMmioTransport::read_interrupt_status() const noexcept {
    if (!m_valid) {
        return 0U;
    }
    return read_reg(REG_INTERRUPT_STATUS);
}

void VirtioMmioTransport::ack_interrupt(uint32_t status_bits) noexcept {
    if (!m_valid || status_bits == 0U) {
        return;
    }
    write_reg(REG_INTERRUPT_ACK, status_bits);
    virtio_mb();
}

uint8_t VirtioMmioTransport::read_config8(size_t offset) const noexcept {
    if (!m_valid || REG_CONFIG_SPACE + offset >= m_region_size) {
        return 0U;
    }
    auto* const reg = reinterpret_cast<volatile uint8_t*>(m_virt_base + REG_CONFIG_SPACE + offset);
    return *reg;
}

uint32_t VirtioMmioTransport::read_config32(size_t offset) const noexcept {
    if (!m_valid || REG_CONFIG_SPACE + offset + sizeof(uint32_t) > m_region_size) {
        return 0U;
    }
    return read_reg(REG_CONFIG_SPACE + offset);
}

uint64_t VirtioMmioTransport::read_config64(size_t offset) const noexcept {
    const uint32_t low = read_config32(offset);
    const uint32_t high = read_config32(offset + 4U);
    return (static_cast<uint64_t>(high) << 32U) | low;
}

uint32_t VirtioMmioTransport::status() const noexcept {
    if (m_virt_base == 0U) {
        return 0U;
    }
    return read_reg(REG_STATUS);
}

} // namespace Kernel::Drivers::Virtio