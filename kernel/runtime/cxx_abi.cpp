#include "hal/cpu.hpp"

#include <cstddef>

extern "C" {
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp)
void* __dso_handle = nullptr;

// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp)
int __cxa_atexit([[maybe_unused]] void (*func)(void*), [[maybe_unused]] void* arg,
                 [[maybe_unused]] void* dso) noexcept {
    return 0; // No-op in bare-metal kernel
}
}

// Standard operator new must not return nullptr.
// Since no heap allocator exists yet, halt execution on accidental dynamic allocation.
void* operator new([[maybe_unused]] size_t size) {
    while (true) {
        HAL::get_cpu().halt();
    }
}

void* operator new[]([[maybe_unused]] size_t size) {
    while (true) {
        HAL::get_cpu().halt();
    }
}

// Placement new (allows constructing objects at pre-allocated buffer addresses)
void* operator new([[maybe_unused]] size_t size, void* ptr) noexcept {
    return ptr;
}

void* operator new[]([[maybe_unused]] size_t size, void* ptr) noexcept {
    return ptr;
}

// Global Memory Deallocation Operators
void operator delete([[maybe_unused]] void* ptr) noexcept {}
void operator delete[]([[maybe_unused]] void* ptr) noexcept {}
void operator delete([[maybe_unused]] void* ptr, [[maybe_unused]] size_t size) noexcept {}
void operator delete[]([[maybe_unused]] void* ptr, [[maybe_unused]] size_t size) noexcept {}