#include "cpu_context.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "hal/targets/qemu_virt/gicv2.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "hal/trap.hpp"
#include "memory_config.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"
#include "process/process.hpp"
#include "process/scheduler.hpp"

#include <cstddef>
#include <cstdint>

extern "C" {
void aarch64_handle_sync_exception(HAL::CpuContext* ctx, uint64_t esr, uint64_t far) noexcept;
void aarch64_handle_irq_exception(HAL::CpuContext* ctx) noexcept;
void aarch64_handle_invalid_exception(HAL::CpuContext* ctx, uint64_t type) noexcept;
[[noreturn]] void return_to_kernel(int64_t exit_code) noexcept;
void* memcpy(void* dest, const void* src, size_t count) noexcept;
}

namespace {

constexpr bool ENABLE_REGISTER_DUMP = false;
constexpr uint64_t TABLE_ADDR_MASK = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t DESC_UXN = 1ULL << 54U;
constexpr uint64_t DESC_COW = 1ULL << 55U;
constexpr uint64_t PTE_DEMAND_ZERO = 1ULL << 56U;
constexpr uint64_t PTE_DZ_FLAGS_SHIFT = 8U;
constexpr uint64_t PTE_DZ_FLAGS_MASK = 0xFFULL << PTE_DZ_FLAGS_SHIFT;

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

void dump_registers(const AArch64CpuContext* arch_ctx) noexcept {
    if constexpr (!ENABLE_REGISTER_DUMP) {
        (void)arch_ctx;
        return;
    }

    if (arch_ctx == nullptr) {
        return;
    }

    print_str("\r\n================ CPU REGISTER DUMP ================\r\n");
    for (int i = 0; i < 31; i += 2) {
        print_str("  X");
        if (i < 10) {
            print_str("0");
        }
        HAL::get_console().putc(static_cast<char>('0' + (i % 10)));
        print_str(": ");
        print_hex64(arch_ctx->x[i]);
        print_str("   ");

        if (i + 1 < 31) {
            print_str("X");
            if (i + 1 < 10) {
                print_str("0");
            }
            HAL::get_console().putc(static_cast<char>('0' + ((i + 1) % 10)));
            print_str(": ");
            print_hex64(arch_ctx->x[i + 1]);
        }
        print_str("\r\n");
    }
    print_str("  SP_EL0  : ");
    print_hex64(arch_ctx->sp_el0);
    print_str("\r\n  ELR_EL1 : ");
    print_hex64(arch_ctx->elr_el1);
    print_str("\r\n  SPSR_EL1: ");
    print_hex64(arch_ctx->spsr_el1);
    print_str("\r\n===================================================\r\n");
}

} // namespace

extern "C" {

void aarch64_handle_irq_exception(HAL::CpuContext* ctx) noexcept {
    const uint32_t irq = HAL::QemuVirt::GicV2::acknowledge_irq();
    if (irq >= 1020U) {
        return; // Spurious interrupt
    }

    auto* const arch_ctx = reinterpret_cast<AArch64CpuContext*>(ctx);
    const bool from_user = (arch_ctx != nullptr) && ((arch_ctx->spsr_el1 & 0x0FU) == 0U);

    if (irq == HAL::QemuVirt::TIMER_PPI_IRQ) {
        HAL::QemuVirt::GenericTimer::reload();
        HAL::QemuVirt::GicV2::end_of_interrupt(irq);
        Kernel::Scheduler::on_timer_tick(from_user);
        return;
    }

    if (from_user) {
        Kernel::Process::deliver_pending_signals(*ctx);
    }

    HAL::QemuVirt::GicV2::end_of_interrupt(irq);
}

void aarch64_handle_sync_exception(HAL::CpuContext* ctx, uint64_t esr, uint64_t far) noexcept {
    const uint32_t eclass = static_cast<uint32_t>((esr >> 26U) & 0x3FU); // Exception Class (EC)
    auto* const arch_ctx = reinterpret_cast<AArch64CpuContext*>(ctx);

    switch (eclass) {
    case 0x15U: { // SVC in AArch64
        HAL::TrapFrame frame{.type = HAL::TrapType::Syscall, .error_code = esr, .context = ctx};
        HAL::get_trap_dispatcher().handle_trap(frame);
        if (ctx != nullptr) {
            Kernel::Process::deliver_pending_signals(*ctx);
        }
        return;
    }

    case 0x20U: // Instruction Abort from Lower EL
    case 0x21U: // Instruction Abort from Current EL
    case 0x24U: // Data Abort from Lower EL
    case 0x25U: // Data Abort from Current EL
    {
        if (eclass == 0x24U || eclass == 0x20U) {
            const uint32_t fsc = static_cast<uint32_t>(esr & 0x3FU);
            const bool is_translation_fault = (fsc & 0x3CU) == 0x04U; // Levels 0..3 (0x04..0x07)
            const bool is_perm_fault = (fsc & 0x3CU) == 0x0CU;        // Levels 0..3 (0x0C..0x0F)
            const bool is_write = (esr & (1ULL << 6U)) != 0ULL;       // WnR bit
            const bool far_valid = (esr & (1ULL << 10U)) == 0ULL;     // FnV == 0

            // 1. Lazy Demand-Zero Translation Fault Resolution
            if (is_translation_fault && far_valid) {
                const uintptr_t current_root = HAL::AddressSpaceBackend::current_root_paddr();
                uint64_t raw_pte = 0ULL;

                if (HAL::AddressSpaceBackend::query_pte_flags(current_root, far, raw_pte) &&
                    ((raw_pte & 1ULL) == 0ULL) && ((raw_pte & PTE_DEMAND_ZERO) != 0ULL)) {
                    const auto intended_flags = static_cast<HAL::PageFlags>(
                        (raw_pte & PTE_DZ_FLAGS_MASK) >> PTE_DZ_FLAGS_SHIFT);

                    const bool perm_ok =
                        HAL::has_flag(intended_flags, HAL::PageFlags::User) &&
                        (eclass != 0x20U ||
                         HAL::has_flag(intended_flags, HAL::PageFlags::Execute)) &&
                        (!is_write || HAL::has_flag(intended_flags, HAL::PageFlags::Write));

                    if (perm_ok) {
                        const uintptr_t new_paddr = alloc_frame();
                        if (new_paddr != 0U) {
                            auto* const dst_kva =
                                reinterpret_cast<void*>(Kernel::MM::phys_to_virt(new_paddr));
                            __builtin_memset(dst_kva, 0, Arch::PAGE_SIZE);
                            HAL::get_cpu().flush_dcache_range(reinterpret_cast<uintptr_t>(dst_kva),
                                                              Arch::PAGE_SIZE);

                            if (HAL::AddressSpaceBackend::map_user_page(
                                    current_root, far, new_paddr, intended_flags)) {
                                if (HAL::has_flag(intended_flags, HAL::PageFlags::Execute)) {
                                    HAL::get_cpu().invalidate_icache();
                                }
                                return; // Resume user execution; faulting instruction retries
                            }
                            free_frame(new_paddr);
                        }
                    }
                }
            }

            // 2. Copy-on-Write (CoW) Write Permission Fault Resolution
            if (eclass == 0x24U && is_perm_fault && is_write && far_valid) {
                const uintptr_t current_root = HAL::AddressSpaceBackend::current_root_paddr();
                uint64_t raw_pte = 0ULL;

                if (HAL::AddressSpaceBackend::query_pte_flags(current_root, far, raw_pte) &&
                    (raw_pte & DESC_COW) != 0ULL) {
                    const uintptr_t fault_paddr = raw_pte & TABLE_ADDR_MASK;
                    Page* const page = page_from_paddr(fault_paddr);

                    if (page != nullptr) {
                        HAL::PageFlags user_rw_flags =
                            HAL::PageFlags::User | HAL::PageFlags::Read | HAL::PageFlags::Write;
                        if ((raw_pte & DESC_UXN) == 0ULL) {
                            user_rw_flags |= HAL::PageFlags::Execute;
                        }
                        if (((raw_pte >> 2U) & 0x7ULL) == 0ULL) {
                            user_rw_flags |= HAL::PageFlags::Device;
                        } else {
                            user_rw_flags |= HAL::PageFlags::Cacheable;
                        }

                        bool cow_resolved = false;
                        if (__atomic_load_n(&page->ref_count, __ATOMIC_ACQ_REL) > 1U) {
                            const uintptr_t new_paddr = alloc_frame();
                            if (new_paddr != 0U) {
                                auto* const dst_kva =
                                    reinterpret_cast<void*>(Kernel::MM::phys_to_virt(new_paddr));
                                const auto* const src_kva = reinterpret_cast<const void*>(
                                    Kernel::MM::phys_to_virt(fault_paddr));
                                memcpy(dst_kva, src_kva, Arch::PAGE_SIZE);
                                HAL::get_cpu().flush_dcache_range(
                                    reinterpret_cast<uintptr_t>(dst_kva), Arch::PAGE_SIZE);

                                if (HAL::AddressSpaceBackend::remap_user_page(
                                        current_root, far, new_paddr, user_rw_flags)) {
                                    page_put(fault_paddr);
                                    cow_resolved = true;
                                } else {
                                    free_frame(new_paddr);
                                }
                            }
                        } else {
                            cow_resolved = HAL::AddressSpaceBackend::remap_user_page(
                                current_root, far, fault_paddr, user_rw_flags);
                        }

                        if (cow_resolved) {
                            asm volatile("dsb ish\n\t"
                                         "tlbi vaae1is, %0\n\t"
                                         "dsb ish\n\t"
                                         "isb" ::"r"(far >> 12U)
                                         : "memory");
                            if (HAL::has_flag(user_rw_flags, HAL::PageFlags::Execute)) {
                                HAL::get_cpu().invalidate_icache();
                            }
                            return; // Resume user execution; faulting instruction retries
                        }
                    }
                }
            }
        }

        print_str("\r\n[TRAP] Data/Instruction Abort Trap Captured!\r\n");
        print_str("  Faulting Virtual Address (FAR_EL1): ");
        print_hex64(far);
        print_str("\r\n  Syndrome Register        (ESR_EL1): ");
        print_hex64(esr);
        print_str("\r\n");

        dump_registers(arch_ctx);

        if (arch_ctx != nullptr && (arch_ctx->spsr_el1 & 0x0FU) == 0U) {
            print_str("[TRAP] Terminating faulting EL0 process.\r\n");
            if (Kernel::Process::current_process() != nullptr) {
                Kernel::Process::exit_current_process(-1);
            }
            return_to_kernel(-1);
        }

        if (arch_ctx != nullptr) {
            if (eclass == 0x21U) {
                print_str(
                    "[TRAP] EL1 Instruction Fault detected! Returning to caller via LR...\r\n");
                arch_ctx->elr_el1 = arch_ctx->x[30];
                return;
            }

            print_str(
                "[TRAP] EL1 Fault detected! Advancing ELR_EL1 past faulting instruction...\r\n");
            arch_ctx->elr_el1 += 4U;
        }
        return;
    }
    default: {
        print_str("\r\n[PANIC] Unhandled Synchronous Exception! EC=");
        print_hex64(eclass);
        print_str(" ESR_EL1=");
        print_hex64(esr);
        print_str(" FAR_EL1=");
        print_hex64(far);
        print_str("\r\n");

        dump_registers(arch_ctx);

        if (arch_ctx != nullptr && (arch_ctx->spsr_el1 & 0x0FU) == 0U) {
            print_str("[TRAP] Terminating faulting EL0 process.\r\n");
            if (Kernel::Process::current_process() != nullptr) {
                Kernel::Process::exit_current_process(-1);
            }
            return_to_kernel(-1);
        }

        while (true) {
            HAL::get_cpu().halt();
        }
    }
    }
}

void aarch64_handle_invalid_exception(HAL::CpuContext* ctx, uint64_t type) noexcept {
    constexpr const char* type_names[] = {"Unknown",
                                          "Current EL SP0 IRQ",
                                          "Current EL SP0 FIQ",
                                          "Current EL SP0 SError",
                                          "Unknown",
                                          "Current EL SPx IRQ",
                                          "Current EL SPx FIQ",
                                          "Current EL SPx SError",
                                          "Unknown",
                                          "Lower EL AArch64 IRQ",
                                          "Lower EL AArch64 FIQ",
                                          "Lower EL AArch64 SError",
                                          "Lower EL AArch32 Sync",
                                          "Lower EL AArch32 IRQ",
                                          "Lower EL AArch32 FIQ",
                                          "Lower EL AArch32 SError"};

    const auto* const arch_ctx = reinterpret_cast<const AArch64CpuContext*>(ctx);

    print_str("\r\n[PANIC] Invalid/Async Trap Captured: ");
    if (type < 16U) {
        print_str(type_names[type]);
    } else {
        print_hex64(type);
    }
    print_str("\r\n");

    dump_registers(arch_ctx);

    if (arch_ctx != nullptr && (arch_ctx->spsr_el1 & 0x0FU) == 0U) {
        print_str("[TRAP] Terminating faulting EL0 process.\r\n");
        if (Kernel::Process::current_process() != nullptr) {
            Kernel::Process::exit_current_process(-3);
        }
        return_to_kernel(-3);
    }

    while (true) {
        HAL::get_cpu().halt();
    }
}

} // extern "C"