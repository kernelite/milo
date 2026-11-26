#include "boot/fdt.hpp"
#include "drivers/block/block_device.hpp"
#include "drivers/irq/gicv2.hpp"
#include "drivers/virtio/virtio_blk.hpp"
#include "drivers/virtio/virtio_mmio.hpp"
#include "fs/devfs/devfs.hpp"
#include "fs/ext2/ext2.hpp"
#include "fs/rootfs/rootfs.hpp"
#include "fs/tmpfs/tmpfs.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "hal/platform.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "mm/heap.hpp"
#include "mm/pfa.hpp"
#include "process/Process.hpp"
#include "process/scheduler.hpp"
#include "shell/shell.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

uintptr_t g_dtb_paddr = 0;
Kernel::Boot::InitrdLocation g_initrd_loc{};

extern "C" {
extern char _text_start[];
// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern char __bss_end[];
}

namespace {

void print_str(const char* str) noexcept {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    HAL::get_console().write(str, len);
}

void print_hex64(uint64_t val) noexcept {
    char buf[19] = "0x0000000000000000";
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 17; i >= 2; --i) {
        buf[i] = hex_chars[val & 0xFU];
        val >>= 4U;
    }
    print_str(buf);
}

Kernel::Drivers::Virtio::VirtioMmioTransport
    g_virtio_transports[Kernel::Boot::VirtioMmioList::MAX_DEVICES]{};

void default_virtio_spi_handler(uint32_t /*irq*/, void* context) noexcept {
    auto* const transport = static_cast<Kernel::Drivers::Virtio::VirtioMmioTransport*>(context);
    if (transport != nullptr && transport->is_valid()) {
        const uint32_t isr = transport->read_interrupt_status();
        if (isr != 0U) {
            transport->ack_interrupt(isr);
        }
    }
}

void init_virtio_mmio_devices(uintptr_t dtb_paddr) noexcept {
    const Kernel::Boot::VirtioMmioList mmio_list =
        Kernel::Boot::find_virtio_mmio_devices(dtb_paddr);
    size_t active_count = 0U;

    for (size_t idx = 0U; idx < mmio_list.count; ++idx) {
        const Kernel::Boot::VirtioMmioDevice& dev = mmio_list.devices[idx];
        auto& transport = g_virtio_transports[idx];
        if (!transport.init(dev.base_paddr, dev.size)) {
            continue;
        }

        ++active_count;
        print_str("[VIRTIO] MMIO slot ");
        print_hex64(dev.base_paddr);
        print_str(" DeviceID=");
        print_hex64(transport.device_id());
        print_str(" Status=DRIVER_OK");

        if (dev.has_irq) {
            const auto trigger = ((dev.irq_trigger & 0x3U) != 0U)
                                     ? Kernel::Drivers::IRQ::IrqTriggerMode::EdgeRising
                                     : Kernel::Drivers::IRQ::IrqTriggerMode::LevelHigh;
            if (Kernel::Drivers::IRQ::GicV2::register_spi_handler(
                    dev.irq_intid, default_virtio_spi_handler, &transport, trigger)) {
                print_str(" IRQ=");
                print_hex64(dev.irq_intid);
            }
        }
        print_str("\r\n");
    }

    if (active_count == 0U) {
        print_str("[VIRTIO] No active Virtio-MMIO v1.0+ devices attached.\r\n");
    }
}

void verify_primary_block_device() noexcept {
    Kernel::Drivers::Block::BlockDevice* const blk = HAL::Platform::primary_block_device();
    if (blk == nullptr) {
        print_str("[block] No block device detected\r\n");
        return;
    }

    const size_t sec_size = blk->sector_size();
    if (sec_size != 512U || blk->sector_count() == 0ULL) {
        print_str("[block] Invalid block device geometry\r\n");
        return;
    }

    alignas(16) uint8_t tx_buf[512]{};
    alignas(16) uint8_t rx_buf[512]{};

    for (size_t i = 0U; i < sizeof(tx_buf); ++i) {
        tx_buf[i] = static_cast<uint8_t>((i * 3U) ^ 0xA5U);
    }

    const int write_rc = blk->write_sectors(0ULL, 1U, tx_buf);
    if (write_rc != 0) {
        print_str("[block] Sector write failed\r\n");
        return;
    }

    const int read_rc = blk->read_sectors(0ULL, 1U, rx_buf);
    if (read_rc != 0) {
        print_str("[block] Sector read failed\r\n");
        return;
    }

    if (std::memcmp(tx_buf, rx_buf, sizeof(tx_buf)) == 0) {
        print_str("[block] Raw sector read/write verification PASSED\r\n");
    } else {
        print_str("[block] Raw sector read/write data mismatch\r\n");
    }
}
} // namespace

extern "C" void kmain(uintptr_t dtb_paddr) {
    g_dtb_paddr = dtb_paddr;

    // Stage 1: Early Console Init (PL011 UART @ 0x09000000 via physical MMIO)
    HAL::get_console().init();
    print_str("[KERNEL] Core HAL initialised successfully.\r\n");
    print_str("[BOOT] DTB Pointer: ");
    print_hex64(dtb_paddr);
    print_str("\r\n");

    // Stage 2: DTB Discovery (Pre-MMU)
    size_t dtb_size = 0U;
    const bool dtb_valid = Kernel::Boot::validate_header(dtb_paddr, &dtb_size);
    const Kernel::Boot::MemoryMap mem_map = Kernel::Boot::find_memory_regions(dtb_paddr);
    const Kernel::Boot::ReserveMap reserve_map = Kernel::Boot::find_memory_reservations(dtb_paddr);
    g_initrd_loc = Kernel::Boot::find_initrd(dtb_paddr);

    for (size_t i = 0U; i < mem_map.count; ++i) {
        print_str("[FDT] Memory Bank: ");
        print_hex64(mem_map.regions[i].base_paddr);
        print_str(" - ");
        print_hex64(mem_map.regions[i].base_paddr + mem_map.regions[i].size);
        print_str("\r\n");
    }
    print_str("[FDT] Discovered RAM Span: ");
    print_hex64(mem_map.min_paddr);
    print_str(" - ");
    print_hex64(mem_map.max_paddr);
    print_str("\r\n");

    // Stage 3: Platform Info Update
    const size_t discovered_ram_span = static_cast<size_t>(mem_map.max_paddr - mem_map.min_paddr);
    HAL::set_platform_memory(mem_map.min_paddr, discovered_ram_span);

    // Stage 4: Dynamic PFA Initialization (Pre-MMU)
    const uintptr_t kernel_start = reinterpret_cast<uintptr_t>(_text_start);
    const uintptr_t kernel_end = reinterpret_cast<uintptr_t>(__bss_end);
    Kernel::MM::pfa_init(mem_map, kernel_start, kernel_end, dtb_valid ? dtb_paddr : 0U,
                         dtb_valid ? dtb_size : 0U,
                         g_initrd_loc.valid ? g_initrd_loc.start_paddr : 0U,
                         g_initrd_loc.valid ? g_initrd_loc.end_paddr : 0U, &reserve_map);

    // Stage 5: Full-RAM MMU Activation & PFA Higher-Half Pointer Rebasing
    print_str("[KERNEL] Activating MMU and Translation Tables...\r\n");
    HAL::get_mmu().activate();
    Kernel::MM::pfa_on_mmu_enabled();
    print_str("[KERNEL] MMU Activated successfully.\r\n");

    // Stage 6: Kernel Slab & Compound Heap Allocator Initialization
    Kernel::MM::heap_init();
    print_str("[KERNEL] Kernel Heap Allocator initialized.\r\n");

    // Stage 6b: Mount read-only CPIO rootfs at `/` and devfs at `/dev`
    if (Kernel::FS::rootfs_mount_from_initrd(g_initrd_loc) == Kernel::FS::kOk &&
        Kernel::FS::devfs_init_and_mount() == Kernel::FS::kOk &&
        Kernel::FS::tmpfs_init_and_mount() == Kernel::FS::kOk) {
        print_str("[KERNEL] VFS rootfs (/), devfs (/dev), and tmpfs (/tmp) mounted.\r\n");
    } else {
        print_str("[KERNEL] Warning: VFS initialization failed.\r\n");
    }

    // Stage 7: Initialize GICv2, 100Hz Timer, and Preemptive Scheduler
    Kernel::Drivers::IRQ::GicV2::init();
    HAL::QemuVirt::GenericTimer::init(100U);
    Kernel::Drivers::IRQ::GicV2::enable_irq(Kernel::Drivers::IRQ::TIMER_PPI_IRQ);
    Kernel::Scheduler::init();
    print_str("[KERNEL] GICv2 & 100Hz Preemptive Scheduler initialized.\r\n");
    init_virtio_mmio_devices(dtb_paddr);
    print_str("[KERNEL] Virtio-MMIO driver attached.");
    HAL::Platform::init_block_devices(dtb_paddr);
    if (auto* const blk = HAL::Platform::primary_block_device(); blk != nullptr) {
        if (Kernel::FS::ext2_mount(blk, "/mnt/disk") == Kernel::FS::kOk) {
            print_str("[KERNEL] Mounted ext2 block device at /mnt/disk.\r\n");
        } else {
            print_str("[KERNEL] Warning: Primary block device is not a valid ext2 image.\r\n");
        }
    } else {
        print_str("[KERNEL] No primary block device detected.\r\n");
    }
    verify_primary_block_device();
    print_str("[KERNEL] Block device driver attached.");

    // Stage 8: Drop into interactive kernel shell
    HAL::get_cpu().disable_interrupts();
    Kernel::Shell::run();
}