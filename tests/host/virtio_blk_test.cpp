#include "drivers/virtio/virtio_blk.hpp"
#include "memory_config.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"
#include "mocks/host_hal.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <snitch/snitch.hpp>
#include <vector>

namespace {

using namespace Kernel::Drivers::Virtio;

constexpr size_t MMIO_MAGIC_VALUE = 0x000U;
constexpr size_t MMIO_VERSION = 0x004U;
constexpr size_t MMIO_DEVICE_ID = 0x008U;
constexpr size_t MMIO_VENDOR_ID = 0x00CU;
constexpr size_t MMIO_DEVICE_FEATURES = 0x010U;
constexpr size_t MMIO_QUEUE_NUM_MAX = 0x034U;
constexpr size_t MMIO_INTERRUPT_STATUS = 0x060U;
constexpr size_t MMIO_STATUS = 0x070U;
constexpr size_t MMIO_CONFIG = 0x100U;

void init_blk_test_pfa() noexcept {
    HostTest::reset_host_hal();
    HAL::get_mmu().activate();

    Kernel::Boot::MemoryMap mem_map{};
    mem_map.regions[0] = Kernel::Boot::MemoryRegion{
        .base_paddr = HostTest::SIM_RAM_BASE,
        .size = HostTest::SIM_RAM_SIZE,
    };
    mem_map.count = 1U;
    mem_map.min_paddr = HostTest::SIM_RAM_BASE;
    mem_map.max_paddr = HostTest::SIM_RAM_BASE + HostTest::SIM_RAM_SIZE;
    mem_map.total_usable_bytes = HostTest::SIM_RAM_SIZE;

    Kernel::MM::pfa_init(mem_map, HostTest::SIM_RAM_BASE,
                         HostTest::SIM_RAM_BASE + Arch::AArch64::PAGE_SIZE, 0U, 0U, 0U, 0U,
                         nullptr);
}

struct RecordedRequest {
    uint32_t type{0U};
    uint64_t sector{0ULL};
    uint32_t data_len{0U};
    uint16_t desc0_flags{0U};
    uint16_t desc1_flags{0U};
    uint16_t desc2_flags{0U};
};

class SimulatedVirtioBlkBackend {
  public:
    explicit SimulatedVirtioBlkBackend(uint64_t capacity_sectors, uint16_t max_queue_size = 64U)
        : m_storage(static_cast<size_t>(capacity_sectors) * 512U, 0U),
          m_capacity_sectors(capacity_sectors) {
        m_mmio_paddr = Kernel::MM::alloc_frame();
        REQUIRE(m_mmio_paddr != 0U);
        m_mmio_regs = reinterpret_cast<volatile uint8_t*>(Kernel::MM::phys_to_virt(m_mmio_paddr));
        std::memset(const_cast<uint8_t*>(m_mmio_regs), 0, Arch::AArch64::PAGE_SIZE);

        write_reg32(MMIO_MAGIC_VALUE, VIRTIO_MMIO_MAGIC);
        write_reg32(MMIO_VERSION, VIRTIO_MMIO_VERSION_MODERN);
        write_reg32(MMIO_DEVICE_ID, VIRTIO_DEV_ID_BLK);
        write_reg32(MMIO_VENDOR_ID, 0x554D4551U);
        write_reg32(MMIO_QUEUE_NUM_MAX, max_queue_size);
        write_reg32(MMIO_CONFIG + 0U, static_cast<uint32_t>(m_capacity_sectors & 0xFFFFFFFFULL));
        write_reg32(MMIO_CONFIG + 4U, static_cast<uint32_t>(m_capacity_sectors >> 32U));
    }

    ~SimulatedVirtioBlkBackend() noexcept {
        if (m_mmio_paddr != 0U) {
            Kernel::MM::free_frame(m_mmio_paddr);
        }
    }

    void attach(VirtioBlkDevice& dev) noexcept {
        dev.set_host_notify_callback(&SimulatedVirtioBlkBackend::on_notify, this);
    }

    [[nodiscard]] uintptr_t mmio_paddr() const noexcept { return m_mmio_paddr; }
    [[nodiscard]] uint32_t read_reg32(size_t offset) const noexcept {
        return *reinterpret_cast<const volatile uint32_t*>(m_mmio_regs + offset);
    }
    void write_reg32(size_t offset, uint32_t val) noexcept {
        *reinterpret_cast<volatile uint32_t*>(m_mmio_regs + offset) = val;
    }

    void set_forced_status(uint8_t status) noexcept { m_forced_status = status; }
    [[nodiscard]] const std::vector<RecordedRequest>& history() const noexcept { return m_history; }
    [[nodiscard]] const std::vector<uint8_t>& storage() const noexcept { return m_storage; }

  private:
    static void on_notify(VirtioBlkDevice& driver, void* ctx) noexcept {
        auto* const self = static_cast<SimulatedVirtioBlkBackend*>(ctx);
        self->process_and_complete(driver);
    }

    void process_and_complete(VirtioBlkDevice& driver) noexcept {
        SplitVirtqueue& vq = driver.queue();
        const VringDesc* const descs = vq.desc_table();
        const VringAvailHeader* const avail_hdr = vq.avail_header();
        const uint16_t* const avail_ring = vq.avail_ring();
        VringUsedHeader* const used_hdr = vq.used_header();
        VringUsedElem* const used_ring = vq.used_ring();
        const uint16_t qsz = vq.queue_size();

        while (m_last_avail_idx != avail_hdr->idx) {
            const uint16_t head = avail_ring[m_last_avail_idx % qsz];
            const VringDesc& d0 = descs[head];
            REQUIRE((d0.flags & VRING_DESC_F_NEXT) != 0U);
            REQUIRE((d0.flags & VRING_DESC_F_WRITE) == 0U);
            REQUIRE(d0.len == sizeof(VirtioBlkReqHdr));

            const auto* const hdr =
                reinterpret_cast<const VirtioBlkReqHdr*>(Kernel::MM::phys_to_virt(d0.addr));

            if (hdr->type == VIRTIO_BLK_T_FLUSH) {
                const VringDesc& d_status = descs[d0.next];
                REQUIRE((d_status.flags & VRING_DESC_F_NEXT) == 0U);
                REQUIRE((d_status.flags & VRING_DESC_F_WRITE) != 0U);
                auto* const status_ptr =
                    reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(d_status.addr));
                *status_ptr = m_forced_status;

                used_ring[m_used_idx % qsz] = VringUsedElem{.id = head, .len = 1U};
                m_used_idx = static_cast<uint16_t>(m_used_idx + 1U);
                used_hdr->idx = m_used_idx;
                m_last_avail_idx = static_cast<uint16_t>(m_last_avail_idx + 1U);
                continue;
            }

            const VringDesc& d1 = descs[d0.next];
            REQUIRE((d1.flags & VRING_DESC_F_NEXT) != 0U);

            const VringDesc& d2 = descs[d1.next];
            REQUIRE((d2.flags & VRING_DESC_F_NEXT) == 0U);
            REQUIRE((d2.flags & VRING_DESC_F_WRITE) != 0U);
            REQUIRE(d2.len == 1U);

            auto* const data_ptr = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(d1.addr));
            auto* const status_ptr = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(d2.addr));

            m_history.push_back(RecordedRequest{
                .type = hdr->type,
                .sector = hdr->sector,
                .data_len = d1.len,
                .desc0_flags = d0.flags,
                .desc1_flags = d1.flags,
                .desc2_flags = d2.flags,
            });

            uint32_t written_len = 1U;
            if (m_forced_status != VIRTIO_BLK_S_OK) {
                *status_ptr = m_forced_status;
            } else if (hdr->type == VIRTIO_BLK_T_IN) {
                REQUIRE((d1.flags & VRING_DESC_F_WRITE) != 0U);
                std::memcpy(data_ptr, m_storage.data() + static_cast<size_t>(hdr->sector) * 512U,
                            d1.len);
                *status_ptr = VIRTIO_BLK_S_OK;
                written_len = d1.len + 1U;
            } else if (hdr->type == VIRTIO_BLK_T_OUT) {
                REQUIRE((d1.flags & VRING_DESC_F_WRITE) == 0U);
                std::memcpy(m_storage.data() + static_cast<size_t>(hdr->sector) * 512U, data_ptr,
                            d1.len);
                *status_ptr = VIRTIO_BLK_S_OK;
            } else {
                *status_ptr = VIRTIO_BLK_S_UNSUPP;
            }

            used_ring[m_used_idx % qsz] = VringUsedElem{.id = head, .len = written_len};
            m_used_idx = static_cast<uint16_t>(m_used_idx + 1U);
            used_hdr->idx = m_used_idx;
            m_last_avail_idx = static_cast<uint16_t>(m_last_avail_idx + 1U);
        }

        write_reg32(MMIO_INTERRUPT_STATUS, VIRTIO_MMIO_INT_VRING);
        driver.handle_irq();
        CHECK(read_reg32(MMIO_INTERRUPT_STATUS) == 0U);
    }

    uintptr_t m_mmio_paddr{0U};
    volatile uint8_t* m_mmio_regs{nullptr};
    std::vector<uint8_t> m_storage{};
    uint64_t m_capacity_sectors{0ULL};
    uint16_t m_last_avail_idx{0U};
    uint16_t m_used_idx{0U};
    uint8_t m_forced_status{VIRTIO_BLK_S_OK};
    std::vector<RecordedRequest> m_history{};
};

} // namespace

TEST_CASE("VirtioBlkDevice 3-descriptor chains, LBA offsets, and error translation",
          "[virtio][blk]") {
    init_blk_test_pfa();

    SECTION("reads capacity from 0x100 and validates LBA bounds") {
        SimulatedVirtioBlkBackend backend(/*capacity_sectors=*/32ULL);
        backend.write_reg32(MMIO_DEVICE_FEATURES, 1U);

        VirtioBlkDevice blk{};
        REQUIRE(blk.init(backend.mmio_paddr()));
        CHECK(blk.sector_size() == 512U);
        CHECK(blk.sector_count() == 32ULL);
        CHECK((backend.read_reg32(MMIO_STATUS) & VIRTIO_STATUS_DRIVER_OK) != 0U);

        std::array<uint8_t, 512> buf{};
        CHECK(blk.read_sectors(0ULL, 0U, buf.data()) == -EINVAL);
        CHECK(blk.read_sectors(0ULL, 1U, nullptr) == -EINVAL);
        CHECK(blk.read_sectors(32ULL, 1U, buf.data()) == -EINVAL);
        CHECK(blk.write_sectors(31ULL, 2U, buf.data()) == -EINVAL);

        blk.shutdown();
    }

    SECTION("verifies 3-descriptor chain layout, VRING_DESC_F_WRITE, and multi-sector LBA I/O") {
        SimulatedVirtioBlkBackend backend(/*capacity_sectors=*/16ULL);
        backend.write_reg32(MMIO_DEVICE_FEATURES, 1U);

        VirtioBlkDevice blk{};
        REQUIRE(blk.init(backend.mmio_paddr()));
        backend.attach(blk);

        std::array<uint8_t, 1024> tx{};
        for (size_t i = 0U; i < tx.size(); ++i) {
            tx[i] = static_cast<uint8_t>((i + 0x33U) & 0xFFU);
        }

        REQUIRE(blk.write_sectors(4ULL, 2U, tx.data()) == 0);
        REQUIRE(backend.history().size() == 1U);

        const RecordedRequest& wr = backend.history()[0];
        CHECK(wr.type == VIRTIO_BLK_T_OUT);
        CHECK(wr.sector == 4ULL);
        CHECK(wr.data_len == 1024U);
        CHECK(wr.desc0_flags == VRING_DESC_F_NEXT);
        CHECK(wr.desc1_flags == VRING_DESC_F_NEXT); // WRITE clear for writes
        CHECK(wr.desc2_flags == VRING_DESC_F_WRITE);

        CHECK(std::memcmp(backend.storage().data() + 4U * 512U, tx.data(), 1024U) == 0);
        CHECK(backend.storage()[3U * 512U + 511U] == 0U);
        CHECK(backend.storage()[6U * 512U] == 0U);

        std::array<uint8_t, 1024> rx{};
        REQUIRE(blk.read_sectors(4ULL, 2U, rx.data()) == 0);
        REQUIRE(backend.history().size() == 2U);

        const RecordedRequest& rd = backend.history()[1];
        CHECK(rd.type == VIRTIO_BLK_T_IN);
        CHECK(rd.sector == 4ULL);
        CHECK(rd.data_len == 1024U);
        CHECK(rd.desc1_flags == (VRING_DESC_F_NEXT | VRING_DESC_F_WRITE)); // WRITE set for reads
        CHECK(std::memcmp(rx.data(), tx.data(), 1024U) == 0);
        CHECK(blk.irq_completions() == 2ULL);
        CHECK(blk.queue().num_free() == blk.queue().queue_size());

        blk.shutdown();
    }

    SECTION("translates VIRTIO_BLK_S_IOERR to -EIO and VIRTIO_BLK_S_UNSUPP to -ENOTSUP") {
        SimulatedVirtioBlkBackend backend(/*capacity_sectors=*/8ULL);
        backend.write_reg32(MMIO_DEVICE_FEATURES, 1U);

        VirtioBlkDevice blk{};
        REQUIRE(blk.init(backend.mmio_paddr()));
        backend.attach(blk);

        std::array<uint8_t, 512> buf{};
        backend.set_forced_status(VIRTIO_BLK_S_IOERR);
        CHECK(blk.read_sectors(0ULL, 1U, buf.data()) == -EIO);

        backend.set_forced_status(VIRTIO_BLK_S_UNSUPP);
        CHECK(blk.flush() == -ENOTSUP);

        CHECK(blk.irq_completions() == 2ULL);
        CHECK(blk.queue().num_free() == blk.queue().queue_size());

        blk.shutdown();
    }
}