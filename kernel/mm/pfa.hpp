#pragma once

#include "hal/cpu.hpp"
#include <cstddef>
#include <cstdint>

constexpr size_t MAX_PAGES = (512UL * 1024UL * 1024UL) / 4096U;
constexpr size_t BITMAP_SIZE_BYTES = MAX_PAGES / 8U;

extern "C" {
extern char _text_start[];
extern char __bss_end[];
}

void pfa_init(uintptr_t ram_start, size_t ram_size) noexcept;
void pfa_reserve_range(uintptr_t start_paddr, uintptr_t end_paddr) noexcept;
bool pfa_is_page_allocated(uintptr_t paddr) noexcept;

uintptr_t alloc_frame() noexcept;
uintptr_t alloc_frames(size_t count) noexcept;
void free_frame(uintptr_t paddr) noexcept;
void free_frames(uintptr_t paddr, size_t count) noexcept;

size_t pfa_allocated_pages_count() noexcept;
size_t pfa_free_pages_count() noexcept;