#include <cstddef>

// Freestanding C++ ABI stubs required by GCC for virtual destructors and static symbols
extern "C" {
    void* __dso_handle = nullptr;

    int __cxa_atexit(void (*)(void*), void*, void*) {
        return 0; // No-op in bare-metal kernel
    }
}

void operator delete(void*, size_t) noexcept {
    // No-op: Kernel objects are statically allocated in HAL
}

void operator delete(void*) noexcept {
    // No-op
}