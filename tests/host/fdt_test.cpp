#include "boot/fdt.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <snitch/snitch.hpp>
#include <string_view>

namespace {

constexpr uint32_t FDT_MAGIC = 0xD00DFEEDU;
constexpr uint32_t FDT_BEGIN_NODE = 0x00000001U;
constexpr uint32_t FDT_END_NODE = 0x00000002U;
constexpr uint32_t FDT_PROP = 0x00000003U;
constexpr uint32_t FDT_END = 0x00000009U;

class SyntheticDtbBuilder {
  public:
    void begin_node(std::string_view name) noexcept {
        push_be32(FDT_BEGIN_NODE);
        for (char chr : name) {
            push_byte(static_cast<uint8_t>(chr));
        }
        push_byte(0U);
        align4();
    }

    void end_node() noexcept { push_be32(FDT_END_NODE); }

    void prop_u32(std::string_view name, uint32_t value) noexcept {
        const uint32_t nameoff = add_string(name);
        push_be32(FDT_PROP);
        push_be32(4U);
        push_be32(nameoff);
        push_be32(value);
    }

    void prop_u32_array(std::string_view name, const uint32_t* values, size_t count) noexcept {
        const uint32_t nameoff = add_string(name);
        push_be32(FDT_PROP);
        push_be32(static_cast<uint32_t>(count * sizeof(uint32_t)));
        push_be32(nameoff);
        for (size_t idx = 0U; idx < count; ++idx) {
            push_be32(values[idx]);
        }
    }

    void prop_string(std::string_view name, std::string_view str) noexcept {
        const uint32_t nameoff = add_string(name);
        push_be32(FDT_PROP);
        push_be32(static_cast<uint32_t>(str.size() + 1U));
        push_be32(nameoff);
        for (char chr : str) {
            push_byte(static_cast<uint8_t>(chr));
        }
        push_byte(0U);
        align4();
    }

    uintptr_t finalize() noexcept {
        push_be32(FDT_END);

        constexpr size_t HEADER_SIZE = 40U;
        constexpr size_t RSVMAP_SIZE = 16U; // Empty terminator (0, 0)
        const size_t off_rsvmap = HEADER_SIZE;
        const size_t off_struct = off_rsvmap + RSVMAP_SIZE;
        const size_t off_strings = off_struct + m_struct_len;
        const size_t total_size = off_strings + m_strings_len;
        REQUIRE(total_size <= m_blob.size());

        std::memset(m_blob.data(), 0, total_size);
        write_be32(m_blob.data() + 0U, FDT_MAGIC);
        write_be32(m_blob.data() + 4U, static_cast<uint32_t>(total_size));
        write_be32(m_blob.data() + 8U, static_cast<uint32_t>(off_struct));
        write_be32(m_blob.data() + 12U, static_cast<uint32_t>(off_strings));
        write_be32(m_blob.data() + 16U, static_cast<uint32_t>(off_rsvmap));
        write_be32(m_blob.data() + 20U, 17U); // version
        write_be32(m_blob.data() + 24U, 16U); // last_comp_version
        write_be32(m_blob.data() + 28U, 0U);  // boot_cpuid_phys
        write_be32(m_blob.data() + 32U, static_cast<uint32_t>(m_strings_len));
        write_be32(m_blob.data() + 36U, static_cast<uint32_t>(m_struct_len));

        std::memcpy(m_blob.data() + off_struct, m_struct.data(), m_struct_len);
        std::memcpy(m_blob.data() + off_strings, m_strings.data(), m_strings_len);
        return reinterpret_cast<uintptr_t>(m_blob.data());
    }

  private:
    static void write_be32(uint8_t* dst, uint32_t val) noexcept {
        dst[0] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
        dst[1] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
        dst[2] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
        dst[3] = static_cast<uint8_t>(val & 0xFFU);
    }

    void push_byte(uint8_t byte_val) noexcept {
        REQUIRE(m_struct_len < m_struct.size());
        m_struct[m_struct_len++] = byte_val;
    }

    void push_be32(uint32_t val) noexcept {
        push_byte(static_cast<uint8_t>((val >> 24U) & 0xFFU));
        push_byte(static_cast<uint8_t>((val >> 16U) & 0xFFU));
        push_byte(static_cast<uint8_t>((val >> 8U) & 0xFFU));
        push_byte(static_cast<uint8_t>(val & 0xFFU));
    }

    void align4() noexcept {
        while ((m_struct_len & 3U) != 0U) {
            push_byte(0U);
        }
    }

    uint32_t add_string(std::string_view str) noexcept {
        const auto offset = static_cast<uint32_t>(m_strings_len);
        REQUIRE(m_strings_len + str.size() + 1U <= m_strings.size());
        for (char chr : str) {
            m_strings[m_strings_len++] = static_cast<uint8_t>(chr);
        }
        m_strings[m_strings_len++] = 0U;
        return offset;
    }

    std::array<uint8_t, 2048> m_struct{};
    size_t m_struct_len{0U};
    std::array<uint8_t, 512> m_strings{};
    size_t m_strings_len{0U};
    alignas(8) std::array<uint8_t, 4096> m_blob{};
};

} // namespace

TEST_CASE("FDT enumerates virtio,mmio nodes and converts SPIs to GIC INTIDs", "[fdt][virtio]") {
    SyntheticDtbBuilder builder{};
    builder.begin_node("");
    builder.prop_u32("#address-cells", 2U);
    builder.prop_u32("#size-cells", 2U);

    // Slot 0: 0x0a000000, size 0x200, SPI 16 (INTID 48), edge-rising (1)
    builder.begin_node("virtio_mmio@a000000");
    builder.prop_string("compatible", "virtio,mmio");
    const uint32_t reg0[4] = {0x0U, 0x0A000000U, 0x0U, 0x00000200U};
    builder.prop_u32_array("reg", reg0, 4U);
    const uint32_t irq0[3] = {0U, 16U, 1U};
    builder.prop_u32_array("interrupts", irq0, 3U);
    builder.end_node();

    // Slot 31 (emitted out of order): 0x0a003e00, size 0x200, SPI 47 (INTID 79), level-high (4)
    builder.begin_node("virtio_mmio@a003e00");
    builder.prop_string("compatible", "virtio,mmio");
    const uint32_t reg31[4] = {0x0U, 0x0A003E00U, 0x0U, 0x00000200U};
    builder.prop_u32_array("reg", reg31, 4U);
    const uint32_t irq31[3] = {0U, 47U, 4U};
    builder.prop_u32_array("interrupts", irq31, 3U);
    builder.end_node();

    // Disabled virtio,mmio node (must be ignored)
    builder.begin_node("virtio_mmio@a000400");
    builder.prop_string("compatible", "virtio,mmio");
    builder.prop_string("status", "disabled");
    const uint32_t reg_dis[4] = {0x0U, 0x0A000400U, 0x0U, 0x00000200U};
    builder.prop_u32_array("reg", reg_dis, 4U);
    const uint32_t irq_dis[3] = {0U, 18U, 1U};
    builder.prop_u32_array("interrupts", irq_dis, 3U);
    builder.end_node();

    // Non-virtio node (must be ignored)
    builder.begin_node("pl011@9000000");
    builder.prop_string("compatible", "arm,pl011");
    const uint32_t reg_uart[4] = {0x0U, 0x09000000U, 0x0U, 0x00001000U};
    builder.prop_u32_array("reg", reg_uart, 4U);
    builder.end_node();

    builder.end_node();
    const uintptr_t dtb_paddr = builder.finalize();

    FDT::VirtioMmioList devices{};
    REQUIRE(FDT::find_virtio_mmio_devices(dtb_paddr, devices));
    REQUIRE(devices.count == 2U);

    CHECK(devices.devices[0].base_paddr == 0x0A000000ULL);
    CHECK(devices.devices[0].size == 0x200U);
    CHECK(devices.devices[0].has_irq);
    CHECK(devices.devices[0].irq_intid == 48U); // SPI 16 + 32
    CHECK(devices.devices[0].irq_trigger == 1U);

    CHECK(devices.devices[1].base_paddr == 0x0A003E00ULL);
    CHECK(devices.devices[1].size == 0x200U);
    CHECK(devices.devices[1].has_irq);
    CHECK(devices.devices[1].irq_intid == 79U); // SPI 47 + 32
    CHECK(devices.devices[1].irq_trigger == 4U);
}

TEST_CASE("FDT virtio enumeration handles null or corrupt DTB safely", "[fdt][virtio]") {
    FDT::VirtioMmioList devices{};
    CHECK_FALSE(FDT::find_virtio_mmio_devices(0U, devices));
    CHECK(devices.count == 0U);
}