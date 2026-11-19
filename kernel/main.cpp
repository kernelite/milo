#include "boot/fdt.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "hal/targets/qemu_virt/gicv2.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "mm/heap.hpp"
#include "mm/pfa.hpp"
#include "process/scheduler.hpp"
#include "shell/shell.hpp"

#include <cstdint>

uintptr_t g_dtb_paddr = 0;
FDT::InitrdLocation g_initrd_loc{};

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
    const bool dtb_valid = FDT::validate_header(dtb_paddr, &dtb_size);
    const FDT::MemoryMap mem_map = FDT::find_memory_regions(dtb_paddr);
    const FDT::ReserveMap reserve_map = FDT::find_memory_reservations(dtb_paddr);
    g_initrd_loc = FDT::find_initrd(dtb_paddr);

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
    pfa_init(mem_map, kernel_start, kernel_end, dtb_valid ? dtb_paddr : 0U,
             dtb_valid ? dtb_size : 0U, g_initrd_loc.valid ? g_initrd_loc.start_paddr : 0U,
             g_initrd_loc.valid ? g_initrd_loc.end_paddr : 0U, &reserve_map);

    // Stage 5: Full-RAM MMU Activation & PFA Higher-Half Pointer Rebasing
    print_str("[KERNEL] Activating MMU and Translation Tables...\r\n");
    HAL::get_mmu().activate();
    pfa_on_mmu_enabled();
    print_str("[KERNEL] MMU Activated successfully.\r\n");

    // Stage 6: Kernel Slab & Compound Heap Allocator Initialization
    Kernel::MM::heap_init();
    print_str("[KERNEL] Kernel Heap Allocator initialized.\r\n");

    // Stage 7: Initialize GICv2, 100Hz Timer, and Preemptive Scheduler
    HAL::QemuVirt::GicV2::init();
    HAL::QemuVirt::GenericTimer::init(100U);
    HAL::QemuVirt::GicV2::enable_irq(HAL::QemuVirt::TIMER_PPI_IRQ);
    Kernel::Scheduler::init();
    print_str("[KERNEL] GICv2 & 100Hz Preemptive Scheduler initialized.\r\n");

    // Stage 8: Drop into interactive kernel shell
    HAL::get_cpu().disable_interrupts();
    Kernel::Shell::run();
}