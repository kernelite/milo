// arch/cpu_context.hpp
#pragma once
#include <cstdint>

struct CpuRegisters {
    uint64_t x[31];     // General purpose registers x0-x30
    uint64_t sp_el0;    // User stack pointer
    uint64_t elr_el1;   // Exception Link Register (Return PC)
    uint64_t spsr_el1;  // Saved Program Status Register
};

namespace Arch {
    inline void set_vbar(uintptr_t vector_table_addr) {
        asm volatile("msr vbar_el1, %0" :: "r"(vector_table_addr));
    }
    inline uint64_t get_esr() {
        uint64_t val;
        asm volatile("mrs %0, esr_el1" : "=r"(val));
        return val;
    }
    void init_mmu(uintptr_t ttbr0_base, uintptr_t ttbr1_base);
    void switch_context(CpuRegisters** old_regs, CpuRegisters* new_regs);
}