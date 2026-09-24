#pragma once

#include <cstddef>
#include <cstdint>

#include "../arch/memory_config.hpp"

constexpr uintptr_t RAM_BASE = Arch::RAM_BASE;
constexpr size_t PAGE_SIZE = Arch::PAGE_SIZE;
constexpr size_t PAGE_SHIFT = Arch::PAGE_SHIFT;
constexpr size_t MAX_RAM_SIZE = Arch::MAX_RAM_SIZE;

constexpr size_t MAX_PAGES = MAX_RAM_SIZE / PAGE_SIZE;
constexpr size_t BITMAP_SIZE_BYTES = MAX_PAGES / 8U;

extern "C" {
extern char _text_start[];
extern char _text_end[];
extern char _rodata_start[];
extern char _rodata_end[];
extern char _data_start[];
extern char _data_end[];
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp)
extern char __bss_start[];
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp)
extern char __bss_end[];
}

void pfa_init(uintptr_t ram_start, size_t ram_size) noexcept;
uintptr_t alloc_frame() noexcept;
uintptr_t alloc_frames(size_t count) noexcept;
void free_frame(uintptr_t paddr) noexcept;
void free_frames(uintptr_t paddr, size_t count) noexcept;