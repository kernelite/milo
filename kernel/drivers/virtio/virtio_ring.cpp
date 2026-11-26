#include "drivers/virtio/virtio_ring.hpp"

#include "memory_config.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"

#include <cstring>

namespace Kernel::Drivers::Virtio {

namespace {

constexpr size_t align_up(size_t val, size_t align) noexcept {
    return (val + (align - 1U)) & ~(align - 1U);
}

constexpr bool is_power_of_two(uint16_t val) noexcept {
    return val != 0U && (val & static_cast<uint16_t>(val - 1U)) == 0U;
}

} // namespace

SplitVirtqueue::SplitVirtqueue(SplitVirtqueue&& other) noexcept
    : m_base_paddr(other.m_base_paddr), m_num_pages(other.m_num_pages),
      m_desc_paddr(other.m_desc_paddr), m_avail_paddr(other.m_avail_paddr),
      m_used_paddr(other.m_used_paddr), m_queue_size(other.m_queue_size),
      m_free_head(other.m_free_head), m_num_free(other.m_num_free),
      m_last_used_idx(other.m_last_used_idx), m_initialized(other.m_initialized) {
    std::memcpy(m_desc_in_use, other.m_desc_in_use, sizeof(m_desc_in_use));
    other.m_base_paddr = 0U;
    other.m_num_pages = 0U;
    other.m_desc_paddr = 0U;
    other.m_avail_paddr = 0U;
    other.m_used_paddr = 0U;
    other.m_queue_size = 0U;
    other.m_free_head = VRING_INVALID_DESC;
    other.m_num_free = 0U;
    other.m_last_used_idx = 0U;
    other.m_initialized = false;
    std::memset(other.m_desc_in_use, 0, sizeof(other.m_desc_in_use));
}

SplitVirtqueue& SplitVirtqueue::operator=(SplitVirtqueue&& other) noexcept {
    if (this != &other) {
        destroy();
        m_base_paddr = other.m_base_paddr;
        m_num_pages = other.m_num_pages;
        m_desc_paddr = other.m_desc_paddr;
        m_avail_paddr = other.m_avail_paddr;
        m_used_paddr = other.m_used_paddr;
        m_queue_size = other.m_queue_size;
        m_free_head = other.m_free_head;
        m_num_free = other.m_num_free;
        m_last_used_idx = other.m_last_used_idx;
        m_initialized = other.m_initialized;
        std::memcpy(m_desc_in_use, other.m_desc_in_use, sizeof(m_desc_in_use));

        other.m_base_paddr = 0U;
        other.m_num_pages = 0U;
        other.m_desc_paddr = 0U;
        other.m_avail_paddr = 0U;
        other.m_used_paddr = 0U;
        other.m_queue_size = 0U;
        other.m_free_head = VRING_INVALID_DESC;
        other.m_num_free = 0U;
        other.m_last_used_idx = 0U;
        other.m_initialized = false;
        std::memset(other.m_desc_in_use, 0, sizeof(other.m_desc_in_use));
    }
    return *this;
}

bool SplitVirtqueue::init(uint16_t queue_size) noexcept {
    if (!is_power_of_two(queue_size) || queue_size > VIRTQ_MAX_SIZE) {
        return false;
    }

    destroy();

    const size_t desc_bytes = static_cast<size_t>(queue_size) * sizeof(VringDesc);
    const size_t avail_bytes = sizeof(VringAvailHeader) +
                               (static_cast<size_t>(queue_size) * sizeof(uint16_t)) +
                               sizeof(uint16_t);
    const size_t used_offset = align_up(desc_bytes + avail_bytes, 4U);
    const size_t used_bytes = sizeof(VringUsedHeader) +
                              (static_cast<size_t>(queue_size) * sizeof(VringUsedElem)) +
                              sizeof(uint16_t);
    const size_t total_bytes = used_offset + used_bytes;
    const size_t num_pages =
        align_up(total_bytes, Arch::AArch64::PAGE_SIZE) / Arch::AArch64::PAGE_SIZE;

    const uintptr_t base_paddr = Kernel::MM::alloc_frames(num_pages);
    if (base_paddr == 0U) {
        return false;
    }

    auto* const base_kva = reinterpret_cast<void*>(Kernel::MM::phys_to_virt(base_paddr));
    std::memset(base_kva, 0, num_pages * Arch::AArch64::PAGE_SIZE);

    m_base_paddr = base_paddr;
    m_num_pages = num_pages;
    m_desc_paddr = base_paddr;
    m_avail_paddr = base_paddr + desc_bytes;
    m_used_paddr = base_paddr + used_offset;
    m_queue_size = queue_size;
    m_free_head = 0U;
    m_num_free = queue_size;
    m_last_used_idx = 0U;
    m_initialized = true;
    std::memset(m_desc_in_use, 0, sizeof(m_desc_in_use));

    VringDesc* const descs = desc_table();
    for (uint16_t idx = 0U; idx < queue_size; ++idx) {
        descs[idx].next =
            (idx + 1U < queue_size) ? static_cast<uint16_t>(idx + 1U) : VRING_INVALID_DESC;
    }

    virtio_wmb();
    return true;
}

void SplitVirtqueue::destroy() noexcept {
    if (m_base_paddr != 0U && m_num_pages > 0U) {
        Kernel::MM::free_frames(m_base_paddr, m_num_pages);
    }
    m_base_paddr = 0U;
    m_num_pages = 0U;
    m_desc_paddr = 0U;
    m_avail_paddr = 0U;
    m_used_paddr = 0U;
    m_queue_size = 0U;
    m_free_head = VRING_INVALID_DESC;
    m_num_free = 0U;
    m_last_used_idx = 0U;
    m_initialized = false;
    std::memset(m_desc_in_use, 0, sizeof(m_desc_in_use));
}

VringDesc* SplitVirtqueue::desc_table() const noexcept {
    if (!m_initialized) {
        return nullptr;
    }
    return reinterpret_cast<VringDesc*>(Kernel::MM::phys_to_virt(m_desc_paddr));
}

VringAvailHeader* SplitVirtqueue::avail_header() const noexcept {
    if (!m_initialized) {
        return nullptr;
    }
    return reinterpret_cast<VringAvailHeader*>(Kernel::MM::phys_to_virt(m_avail_paddr));
}

uint16_t* SplitVirtqueue::avail_ring() const noexcept {
    if (!m_initialized) {
        return nullptr;
    }
    return reinterpret_cast<uint16_t*>(Kernel::MM::phys_to_virt(m_avail_paddr) +
                                       sizeof(VringAvailHeader));
}

VringUsedHeader* SplitVirtqueue::used_header() const noexcept {
    if (!m_initialized) {
        return nullptr;
    }
    return reinterpret_cast<VringUsedHeader*>(Kernel::MM::phys_to_virt(m_used_paddr));
}

VringUsedElem* SplitVirtqueue::used_ring() const noexcept {
    if (!m_initialized) {
        return nullptr;
    }
    return reinterpret_cast<VringUsedElem*>(Kernel::MM::phys_to_virt(m_used_paddr) +
                                            sizeof(VringUsedHeader));
}

uint16_t SplitVirtqueue::avail_idx() const noexcept {
    const VringAvailHeader* const hdr = avail_header();
    return (hdr != nullptr) ? hdr->idx : 0U;
}

uint16_t SplitVirtqueue::alloc_desc() noexcept {
    if (!m_initialized || m_num_free == 0U || m_free_head >= m_queue_size) {
        return VRING_INVALID_DESC;
    }

    const uint16_t idx = m_free_head;
    VringDesc* const descs = desc_table();
    m_free_head = descs[idx].next;
    --m_num_free;
    m_desc_in_use[idx] = true;

    descs[idx] = VringDesc{};
    return idx;
}

bool SplitVirtqueue::free_desc(uint16_t idx) noexcept {
    if (!m_initialized || idx >= m_queue_size || !m_desc_in_use[idx]) {
        return false;
    }

    VringDesc* const descs = desc_table();
    descs[idx] = VringDesc{};
    descs[idx].next = m_free_head;
    m_free_head = idx;
    m_desc_in_use[idx] = false;
    ++m_num_free;
    return true;
}

size_t SplitVirtqueue::free_desc_chain(uint16_t head_idx) noexcept {
    if (!m_initialized || head_idx >= m_queue_size) {
        return 0U;
    }

    VringDesc* const descs = desc_table();
    size_t freed_count = 0U;
    uint16_t curr = head_idx;

    while (curr < m_queue_size && freed_count < m_queue_size) {
        const uint16_t flags = descs[curr].flags;
        const uint16_t next = descs[curr].next;
        if (!free_desc(curr)) {
            break;
        }
        ++freed_count;
        if ((flags & VRING_DESC_F_NEXT) == 0U) {
            break;
        }
        curr = next;
    }

    return freed_count;
}

bool SplitVirtqueue::submit_chain(const VirtqBuffer* buffers, size_t count,
                                  uint16_t* out_head) noexcept {
    if (!m_initialized || buffers == nullptr || count == 0U || count > m_num_free) {
        return false;
    }

    VringDesc* const descs = desc_table();
    VringAvailHeader* const hdr = avail_header();
    uint16_t* const ring = avail_ring();
    if (descs == nullptr || hdr == nullptr || ring == nullptr) {
        return false;
    }

    uint16_t head_idx = VRING_INVALID_DESC;
    uint16_t prev_idx = VRING_INVALID_DESC;

    for (size_t idx = 0U; idx < count; ++idx) {
        const uint16_t desc_idx = alloc_desc();
        if (desc_idx == VRING_INVALID_DESC) {
            if (head_idx != VRING_INVALID_DESC) {
                (void)free_desc_chain(head_idx);
            }
            return false;
        }

        if (head_idx == VRING_INVALID_DESC) {
            head_idx = desc_idx;
        }
        if (prev_idx != VRING_INVALID_DESC) {
            descs[prev_idx].flags |= VRING_DESC_F_NEXT;
            descs[prev_idx].next = desc_idx;
        }

        descs[desc_idx].addr = static_cast<uint64_t>(buffers[idx].paddr);
        descs[desc_idx].len = buffers[idx].len;
        descs[desc_idx].flags = buffers[idx].device_writable ? VRING_DESC_F_WRITE : 0U;
        descs[desc_idx].next = 0U;
        prev_idx = desc_idx;
    }

    const uint16_t slot = static_cast<uint16_t>(hdr->idx % m_queue_size);
    ring[slot] = head_idx;

    // Ensure descriptor table and avail->ring entries are visible before advancing avail->idx
    virtio_wmb();
    hdr->idx = static_cast<uint16_t>(hdr->idx + 1U);
    virtio_mb();

    if (out_head != nullptr) {
        *out_head = head_idx;
    }
    return true;
}

bool SplitVirtqueue::has_used() const noexcept {
    if (!m_initialized) {
        return false;
    }
    virtio_rmb();
    const VringUsedHeader* const hdr = used_header();
    return hdr != nullptr && m_last_used_idx != hdr->idx;
}

bool SplitVirtqueue::pop_used(uint16_t& out_head_idx, uint32_t& out_written_len,
                              bool auto_free) noexcept {
    if (!m_initialized) {
        return false;
    }

    const VringUsedHeader* const hdr = used_header();
    if (m_last_used_idx == hdr->idx) {
        return false;
    }

    // Order reading used->idx before reading used->ring[slot]
    virtio_rmb();

    const uint16_t slot = static_cast<uint16_t>(m_last_used_idx % m_queue_size);
    const VringUsedElem elem = used_ring()[slot];
    m_last_used_idx = static_cast<uint16_t>(m_last_used_idx + 1U);

    out_head_idx = static_cast<uint16_t>(elem.id);
    out_written_len = elem.len;

    if (auto_free) {
        (void)free_desc_chain(out_head_idx);
    }
    return true;
}

bool SplitVirtqueue::should_notify() const noexcept {
    if (!m_initialized) {
        return false;
    }
    virtio_mb();
    const VringUsedHeader* const hdr = used_header();
    return hdr != nullptr && (hdr->flags & VRING_USED_F_NO_NOTIFY) == 0U;
}

void SplitVirtqueue::suppress_interrupts(bool suppress) const noexcept {
    if (!m_initialized) {
        return;
    }
    VringAvailHeader* const hdr = avail_header();
    if (hdr == nullptr) {
        return;
    }
    if (suppress) {
        hdr->flags |= VRING_AVAIL_F_NO_INTERRUPT;
    } else {
        hdr->flags &= static_cast<uint16_t>(~VRING_AVAIL_F_NO_INTERRUPT);
    }
    virtio_wmb();
}

} // namespace Kernel::Drivers::Virtio