#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Kernel::Drivers::Virtio {

constexpr uint16_t VRING_DESC_F_NEXT = 1U;
constexpr uint16_t VRING_DESC_F_WRITE = 2U;
constexpr uint16_t VRING_DESC_F_INDIRECT = 4U;

constexpr uint16_t VRING_AVAIL_F_NO_INTERRUPT = 1U;
constexpr uint16_t VRING_USED_F_NO_NOTIFY = 1U;

constexpr uint16_t VRING_INVALID_DESC = 0xFFFFU;
constexpr uint16_t VIRTQ_MAX_SIZE = 256U;

struct alignas(16) VringDesc {
    uint64_t addr{0ULL};
    uint32_t len{0U};
    uint16_t flags{0U};
    uint16_t next{0U};
};
static_assert(sizeof(VringDesc) == 16U, "VringDesc must be 16 bytes");

struct alignas(2) VringAvailHeader {
    uint16_t flags{0U};
    uint16_t idx{0U};
};
static_assert(sizeof(VringAvailHeader) == 4U, "VringAvailHeader must be 4 bytes");

struct alignas(4) VringUsedElem {
    uint32_t id{0U};
    uint32_t len{0U};
};
static_assert(sizeof(VringUsedElem) == 8U, "VringUsedElem must be 8 bytes");

struct alignas(4) VringUsedHeader {
    uint16_t flags{0U};
    uint16_t idx{0U};
};
static_assert(sizeof(VringUsedHeader) == 4U, "VringUsedHeader must be 4 bytes");

struct VirtqBuffer {
    uintptr_t paddr{0U};
    uint32_t len{0U};
    bool device_writable{false};
};

inline void virtio_mb() noexcept {
#if defined(__aarch64__) && !defined(HOST_TEST)
    asm volatile("dmb ish" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

inline void virtio_rmb() noexcept {
#if defined(__aarch64__) && !defined(HOST_TEST)
    asm volatile("dmb ishld" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_acquire);
#endif
}

inline void virtio_wmb() noexcept {
#if defined(__aarch64__) && !defined(HOST_TEST)
    asm volatile("dmb ishst" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_release);
#endif
}

class SplitVirtqueue final {
  public:
    constexpr SplitVirtqueue() noexcept = default;
    ~SplitVirtqueue() noexcept { destroy(); }

    SplitVirtqueue(const SplitVirtqueue&) = delete;
    SplitVirtqueue& operator=(const SplitVirtqueue&) = delete;

    SplitVirtqueue(SplitVirtqueue&& other) noexcept;
    SplitVirtqueue& operator=(SplitVirtqueue&& other) noexcept;

    [[nodiscard]] bool init(uint16_t queue_size) noexcept;
    void destroy() noexcept;

    [[nodiscard]] uint16_t alloc_desc() noexcept;
    bool free_desc(uint16_t idx) noexcept;
    size_t free_desc_chain(uint16_t head_idx) noexcept;

    [[nodiscard]] bool submit_chain(const VirtqBuffer* buffers, size_t count,
                                    uint16_t* out_head = nullptr) noexcept;

    [[nodiscard]] bool has_used() const noexcept;
    [[nodiscard]] bool pop_used(uint16_t& out_head_idx, uint32_t& out_written_len,
                                bool auto_free = true) noexcept;

    [[nodiscard]] bool should_notify() const noexcept;
    void suppress_interrupts(bool suppress) const noexcept;

    [[nodiscard]] bool is_initialized() const noexcept { return m_initialized; }
    [[nodiscard]] uint16_t queue_size() const noexcept { return m_queue_size; }
    [[nodiscard]] uint16_t num_free() const noexcept { return m_num_free; }
    [[nodiscard]] uintptr_t desc_paddr() const noexcept { return m_desc_paddr; }
    [[nodiscard]] uintptr_t avail_paddr() const noexcept { return m_avail_paddr; }
    [[nodiscard]] uintptr_t used_paddr() const noexcept { return m_used_paddr; }
    [[nodiscard]] uint16_t avail_idx() const noexcept;
    [[nodiscard]] uint16_t last_used_idx() const noexcept { return m_last_used_idx; }

    [[nodiscard]] VringDesc* desc_table() const noexcept;
    [[nodiscard]] VringAvailHeader* avail_header() const noexcept;
    [[nodiscard]] uint16_t* avail_ring() const noexcept;
    [[nodiscard]] VringUsedHeader* used_header() const noexcept;
    [[nodiscard]] VringUsedElem* used_ring() const noexcept;

  private:
    uintptr_t m_base_paddr{0U};
    size_t m_num_pages{0U};
    uintptr_t m_desc_paddr{0U};
    uintptr_t m_avail_paddr{0U};
    uintptr_t m_used_paddr{0U};
    uint16_t m_queue_size{0U};
    uint16_t m_free_head{VRING_INVALID_DESC};
    uint16_t m_num_free{0U};
    uint16_t m_last_used_idx{0U};
    bool m_initialized{false};
    bool m_desc_in_use[VIRTQ_MAX_SIZE]{};
};

} // namespace Kernel::Drivers::Virtio