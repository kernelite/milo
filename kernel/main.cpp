#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "shell/shell.hpp"

extern "C" void kmain() {
    // 1. Initialize Early I/O Console via HAL Trait
    HAL::console.init();
    HAL::console.write("[KERNEL] Core HAL initialised successfully.\n", 44);

    // 2. CPU Management via HAL Trait (No inline asm!)
    HAL::cpu.disable_interrupts();
    
    if (!HAL::cpu.interrupts_enabled()) {
        HAL::console.write("[KERNEL] Interrupts safely disabled.\n", 37);
    }

    // 3. Kernel shell Loop
    Kernel::Shell::run();
}