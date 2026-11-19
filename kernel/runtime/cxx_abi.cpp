#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "mm/heap.hpp"

#include <cstddef>
#include <new>

void operator delete(void* ptr, std::align_val_t align) noexcept;
void operator delete[](void* ptr, std::align_val_t align) noexcept;
void operator delete(void* ptr, std::size_t size, std::align_val_t align) noexcept;
void operator delete[](void* ptr, std::size_t size, std::align_val_t align) noexcept;

namespace {

[[noreturn]] void kernel_heap_oom_panic() noexcept {
    constexpr char msg[] = "\r\n[PANIC] Kernel heap OOM\r\n";
    HAL::get_console().write(msg, sizeof(msg) - 1U);
    while (true) {
        HAL::get_cpu().halt();
    }
}

} // namespace

namespace std {
const nothrow_t nothrow{};
} // namespace std

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

// ==========================================================================
// Throwing Global Allocation Operators (Panic & Halt on OOM)
// ==========================================================================

void* operator new(size_t size) {
    void* const ptr = Kernel::MM::kmalloc(size, 16U);
    if (ptr == nullptr) {
        kernel_heap_oom_panic();
    }
    return ptr;
}

void* operator new[](size_t size) {
    void* const ptr = Kernel::MM::kmalloc(size, 16U);
    if (ptr == nullptr) {
        kernel_heap_oom_panic();
    }
    return ptr;
}

void* operator new(size_t size, std::align_val_t align) {
    void* const ptr = Kernel::MM::kmalloc(size, static_cast<size_t>(align));
    if (ptr == nullptr) {
        kernel_heap_oom_panic();
    }
    return ptr;
}

void* operator new[](size_t size, std::align_val_t align) {
    void* const ptr = Kernel::MM::kmalloc(size, static_cast<size_t>(align));
    if (ptr == nullptr) {
        kernel_heap_oom_panic();
    }
    return ptr;
}

// ==========================================================================
// Non-Throwing Global Allocation Operators (Return nullptr on OOM)
// ==========================================================================

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    return Kernel::MM::kmalloc(size, 16U);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    return Kernel::MM::kmalloc(size, 16U);
}

void* operator new(size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return Kernel::MM::kmalloc(size, static_cast<size_t>(align));
}

void* operator new[](size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return Kernel::MM::kmalloc(size, static_cast<size_t>(align));
}

// ==========================================================================
// Global Deallocation Operators
// ==========================================================================

void operator delete(void* ptr) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete[](void* ptr) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete(void* ptr, [[maybe_unused]] size_t size) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete[](void* ptr, [[maybe_unused]] size_t size) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete(void* ptr, [[maybe_unused]] std::align_val_t align) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete[](void* ptr, [[maybe_unused]] std::align_val_t align) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete(void* ptr, [[maybe_unused]] size_t size,
                     [[maybe_unused]] std::align_val_t align) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete[](void* ptr, [[maybe_unused]] size_t size,
                       [[maybe_unused]] std::align_val_t align) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete(void* ptr, [[maybe_unused]] std::align_val_t align,
                     const std::nothrow_t&) noexcept {
    Kernel::MM::kfree(ptr);
}

void operator delete[](void* ptr, [[maybe_unused]] std::align_val_t align,
                       const std::nothrow_t&) noexcept {
    Kernel::MM::kfree(ptr);
}