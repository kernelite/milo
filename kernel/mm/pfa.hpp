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

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern char _text_start[];
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern char __bss_end[];
}

void pfa_init(uintptr_t ram_start, size_t ram_size) noexcept;
void pfa_reserve_range(uintptr_t start_paddr, uintptr_t end_paddr) noexcept;
bool pfa_is_page_allocated(uintptr_t paddr) noexcept;

uintptr_t alloc_frame() noexcept;
uintptr_t alloc_frames(size_t count) noexcept;
void free_frame(uintptr_t paddr) noexcept;
void free_frames(uintptr_t paddr, size_t count) noexcept;