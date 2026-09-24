#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "mm/pfa.hpp"
#include "shell/shell.hpp"

extern "C" {
extern void user_space_code();
extern void trigger_svc_test();
}

namespace {

void init_kernel_mmu() {
    using HAL::PageFlags;

    const uintptr_t text_start   = reinterpret_cast<uintptr_t>(_text_start) & ~(PAGE_SIZE - 1U);
    const uintptr_t text_end     = (reinterpret_cast<uintptr_t>(_text_end) + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
    const uintptr_t rodata_start = reinterpret_cast<uintptr_t>(_rodata_start) & ~(PAGE_SIZE - 1U);
    const uintptr_t rodata_end   = (reinterpret_cast<uintptr_t>(_rodata_end) + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
    const uintptr_t data_start   = reinterpret_cast<uintptr_t>(_data_start) & ~(PAGE_SIZE - 1U);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const uintptr_t bss_end_addr = (reinterpret_cast<uintptr_t>(__bss_end) + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);

    constexpr uintptr_t HIGHER_HALF_OFFSET = 0xFFFF000000000000ULL;

    // 1. Map .text section (RO + Executable)
    for (uintptr_t addr = text_start; addr < text_end; addr += PAGE_SIZE) {
        HAL::get_mmu().map(addr, addr, PageFlags::Read | PageFlags::Execute);
        HAL::get_mmu().map(HIGHER_HALF_OFFSET + addr, addr, PageFlags::Read | PageFlags::Execute);
    }

    // 2. Map .rodata section (RO + NX)
    for (uintptr_t addr = rodata_start; addr < rodata_end; addr += PAGE_SIZE) {
        HAL::get_mmu().map(addr, addr, PageFlags::Read);
        HAL::get_mmu().map(HIGHER_HALF_OFFSET + addr, addr, PageFlags::Read);
    }

    // 3. Map .data / .bss sections (RW + NX)
    for (uintptr_t addr = data_start; addr < bss_end_addr; addr += PAGE_SIZE) {
        HAL::get_mmu().map(addr, addr, PageFlags::Read | PageFlags::Write);
        HAL::get_mmu().map(HIGHER_HALF_OFFSET + addr, addr, PageFlags::Read | PageFlags::Write);
    }

    // 4. Map FULL platform RAM up to 512 MB (RW + NX)
    const uintptr_t ram_end = Arch::RAM_BASE + Arch::MAX_RAM_SIZE;
    for (uintptr_t addr = bss_end_addr; addr < ram_end; addr += PAGE_SIZE) {
        HAL::get_mmu().map(addr, addr, PageFlags::Read | PageFlags::Write);
        HAL::get_mmu().map(HIGHER_HALF_OFFSET + addr, addr, PageFlags::Read | PageFlags::Write);
    }

    // 5. Map UART MMIO region @ 0x09000000 (Device RW + NX)
    constexpr uintptr_t UART_BASE = 0x09000000U;
    HAL::get_mmu().map(UART_BASE, UART_BASE, PageFlags::Read | PageFlags::Write | PageFlags::Device);
    HAL::get_mmu().map(HIGHER_HALF_OFFSET + UART_BASE, UART_BASE, PageFlags::Read | PageFlags::Write | PageFlags::Device);

    // 6. Map EL0 user space routines with User permission
    const uintptr_t user_code_page = reinterpret_cast<uintptr_t>(user_space_code) & ~(PAGE_SIZE - 1U);
    HAL::get_mmu().map(user_code_page, user_code_page, PageFlags::Read | PageFlags::Execute | PageFlags::User);

    const uintptr_t svc_code_page = reinterpret_cast<uintptr_t>(trigger_svc_test) & ~(PAGE_SIZE - 1U);
    HAL::get_mmu().map(svc_code_page, svc_code_page, PageFlags::Read | PageFlags::Execute | PageFlags::User);

    // 7. Activate MMU
    HAL::get_mmu().activate();
}

} // namespace

extern "C" void kmain() {
    HAL::get_console().init();
    HAL::get_console().write("[KERNEL] Core HAL initialised successfully.\n", 44);

    HAL::get_cpu().disable_interrupts();

    if (!HAL::get_cpu().interrupts_enabled()) {
        HAL::get_console().write("[KERNEL] Interrupts safely disabled.\n", 37);
    }

    pfa_init(Arch::RAM_BASE, Arch::MAX_RAM_SIZE);

    init_kernel_mmu();

    const auto status = HAL::get_mmu().status();
    if (status.mmu_enabled) {
        HAL::get_console().write("[KERNEL] AArch64 MMU & Caches successfully active!\n", 51);
    }

    Kernel::Shell::run();
}