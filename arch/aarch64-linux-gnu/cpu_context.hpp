#pragma once

#include "hal/cpu.hpp"

#include <cstdint>

struct alignas(16) AArch64CpuContext {
    uint64_t x[31];     // General purpose registers x0-x30 (offsets 0..240)
    uint64_t sp_el0;    // User stack pointer (offset 248)
    uint64_t elr_el1;   // Exception Link Register (offset 256)
    uint64_t spsr_el1;  // Saved Program Status Register (offset 264)
    uint64_t tpidr_el0; // EL0 Thread Pointer / TLS base (offset 272)
    uint64_t _pad;      // 16-byte alignment padding (offset 280)
};

static_assert(sizeof(AArch64CpuContext) == sizeof(HAL::CpuContext),
              "AArch64CpuContext must match HAL::CpuContext size");
static_assert(alignof(AArch64CpuContext) == alignof(HAL::CpuContext),
              "AArch64CpuContext must match HAL::CpuContext alignment");

struct alignas(16) AArch64KernelContext {
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;
    uint64_t x30;
    uint64_t sp;
    uint64_t _pad;
};

static_assert(sizeof(AArch64KernelContext) == sizeof(HAL::KernelContext),
              "AArch64KernelContext must match HAL::KernelContext size");
static_assert(alignof(AArch64KernelContext) == alignof(HAL::KernelContext),
              "AArch64KernelContext must match HAL::KernelContext alignment");

namespace Arch {
inline void set_vbar(uintptr_t vector_table_addr) {
    asm volatile("msr vbar_el1, %0" ::"r"(vector_table_addr));
}
inline uint64_t get_esr() {
    uint64_t val;
    asm volatile("mrs %0, esr_el1" : "=r"(val));
    return val;
}
void init_mmu(uintptr_t ttbr0_base, uintptr_t ttbr1_base);
} // namespace Arch