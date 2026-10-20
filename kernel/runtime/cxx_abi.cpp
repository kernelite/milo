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

void* memset(void* dest, int c, size_t n) noexcept {
    auto* p = static_cast<unsigned char*>(dest);
    const auto val = static_cast<unsigned char>(c);
    for (size_t i = 0; i < n; ++i) {
        p[i] = val;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t n) noexcept {
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) noexcept {
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
    if (d < s) {
        for (size_t i = 0; i < n; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) noexcept {
    const auto* p1 = static_cast<const unsigned char*>(s1);
    const auto* p2 = static_cast<const unsigned char*>(s2);
    for (size_t i = 0; i < n; ++i) {
        if (p1[i] != p2[i]) {
            return (p1[i] < p2[i]) ? -1 : 1;
        }
    }
    return 0;
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