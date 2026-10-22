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

void* memset(void* dest, int byte_val, size_t count) noexcept {
    auto* dest_ptr = static_cast<unsigned char*>(dest);
    const auto val = static_cast<unsigned char>(byte_val);
    for (size_t i = 0; i < count; ++i) {
        dest_ptr[i] = val;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t count) noexcept {
    auto* dest_ptr = static_cast<unsigned char*>(dest);
    const auto* src_ptr = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < count; ++i) {
        dest_ptr[i] = src_ptr[i];
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t count) noexcept {
    auto* dest_ptr = static_cast<unsigned char*>(dest);
    const auto* src_ptr = static_cast<const unsigned char*>(src);
    if (dest_ptr < src_ptr) {
        for (size_t i = 0; i < count; ++i) {
            dest_ptr[i] = src_ptr[i];
        }
    } else if (dest_ptr > src_ptr) {
        for (size_t i = count; i > 0; --i) {
            dest_ptr[i - 1] = src_ptr[i - 1];
        }
    }
    return dest;
}

int memcmp(const void* str1, const void* str2, size_t count) noexcept {
    const auto* ptr1 = static_cast<const unsigned char*>(str1);
    const auto* ptr2 = static_cast<const unsigned char*>(str2);
    for (size_t i = 0; i < count; ++i) {
        if (ptr1[i] != ptr2[i]) {
            return (ptr1[i] < ptr2[i]) ? -1 : 1;
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