// kernel/main.cpp
#include "hal/console.hpp"

extern "C" void kmain(void) {
    // Microkernel initialization logic
    HAL::console.write("Kernel booting...\n", 18);

    while (true) {
        // Halt or idle loop
    }
}
