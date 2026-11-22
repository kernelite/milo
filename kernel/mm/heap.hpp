#pragma once

#include <cstddef>

namespace Kernel::MM {

struct HeapStats {
    size_t active_slab_objects{0U};
    size_t slab_pages{0U};
    size_t compound_pages{0U};
    size_t active_bytes{0U};
};

void heap_init() noexcept;

[[nodiscard]] void* kmalloc(size_t size, size_t align = 16U) noexcept;
[[nodiscard]] void* kzalloc(size_t size, size_t align = 16U) noexcept;
[[nodiscard]] void* krealloc(void* ptr, size_t new_size, size_t align = 16U) noexcept;
void kfree(void* ptr) noexcept;

[[nodiscard]] HeapStats heap_get_stats() noexcept;
[[nodiscard]] bool run_concurrent_refcount_test() noexcept;

} // namespace Kernel::MM