#include "hal/cpu.hpp"
#include "process/stack_builder.hpp"
#include "syscalls/SyscallInternal.hpp"

namespace Kernel::Syscalls {

int64_t sys_brk(uintptr_t addr) noexcept {
    auto* const proc = Kernel::Process::Process::current();
    if (proc == nullptr) {
        return 0;
    }

    if (addr == 0U || addr < proc->brk_start()) {
        return static_cast<int64_t>(proc->brk_current());
    }

    const auto platform = HAL::get_platform_info();
    const uintptr_t stack_floor =
        platform.user_stack_top - Kernel::Process::DEFAULT_USER_STACK_SIZE;
    uintptr_t brk_ceiling = (MMAP_ARENA_FLOOR < stack_floor) ? MMAP_ARENA_FLOOR : stack_floor;
    if (proc->mmap_next_vaddr() != 0U && proc->mmap_next_vaddr() < brk_ceiling) {
        brk_ceiling = proc->mmap_next_vaddr();
    }

    if (addr > brk_ceiling) {
        return static_cast<int64_t>(proc->brk_current());
    }

    const size_t page_size = platform.page_size;
    const uintptr_t page_mask = page_size - 1U;
    const uintptr_t old_page_end = (proc->brk_current() + page_mask) & ~page_mask;
    const uintptr_t new_page_end = (addr + page_mask) & ~page_mask;

    if (new_page_end > old_page_end) {
        const HAL::PageFlags heap_flags =
            HAL::PageFlags::User | HAL::PageFlags::Read | HAL::PageFlags::Write;

        for (uintptr_t page_vaddr = old_page_end; page_vaddr < new_page_end;
             page_vaddr += page_size) {
            if (!proc->address_space().map_demand_zero(page_vaddr, heap_flags)) {
                unmap_and_free_range(proc->address_space(), old_page_end, page_vaddr, page_size);
                return static_cast<int64_t>(proc->brk_current());
            }
        }
    } else if (new_page_end < old_page_end) {
        unmap_and_free_range(proc->address_space(), new_page_end, old_page_end, page_size);
    }

    proc->set_brk_current(addr);
    return static_cast<int64_t>(proc->brk_current());
}

int64_t sys_mmap(uintptr_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t raw_fd,
                 uint64_t offset) noexcept {
    constexpr uintptr_t HIGH_MMAP_ARENA_FLOOR = 0x0000000080000000ULL;
    constexpr uintptr_t HIGH_MMAP_SEARCH_STEP = 0x0000000000200000ULL; // 2 MiB L2 step

    auto* const proc = Kernel::Process::Process::current();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const auto platform = HAL::get_platform_info();
    const size_t page_size = platform.page_size;
    const uintptr_t page_mask = page_size - 1U;
    const uintptr_t va_limit = Kernel::MM::user_addr_space_limit();
    const uintptr_t stack_floor =
        platform.user_stack_top - Kernel::Process::DEFAULT_USER_STACK_SIZE;

    if (length == 0U || (length & page_mask) != 0U) {
        return ERR_EINVAL;
    }
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0U) {
        return ERR_EINVAL;
    }
    if ((flags & MAP_ANONYMOUS) == 0U || (flags & MAP_PRIVATE) == 0U) {
        return ERR_EINVAL;
    }
    if ((flags & MAP_SHARED) != 0U) {
        return ERR_EINVAL;
    }
    if (static_cast<int64_t>(raw_fd) != -1 || offset != 0U) {
        return ERR_EINVAL;
    }

    const bool is_fixed = (flags & MAP_FIXED) != 0U;
    uintptr_t map_start = 0U;
    uintptr_t map_end = 0U;
    bool used_arena_bump = false;

    auto is_valid_mmap_span = [&](uintptr_t start, uintptr_t end) noexcept -> bool {
        const bool in_lower = (start >= MMAP_ARENA_FLOOR && end <= stack_floor);
        const bool in_high = (start >= HIGH_MMAP_ARENA_FLOOR && end <= va_limit);
        return in_lower || in_high;
    };

    if (is_fixed) {
        if (addr == 0U || (addr & page_mask) != 0U || addr >= va_limit ||
            length > (va_limit - addr)) {
            return ERR_EINVAL;
        }
        map_start = addr;
        map_end = addr + static_cast<uintptr_t>(length);
        if (!is_valid_mmap_span(map_start, map_end)) {
            return ERR_ENOMEM;
        }
        unmap_and_free_range(proc->address_space(), map_start, map_end, page_size);
    } else if (addr != 0U && (addr & page_mask) == 0U && addr < va_limit &&
               length <= (va_limit - addr) &&
               is_valid_mmap_span(addr, addr + static_cast<uintptr_t>(length))) {
        bool hint_free = true;
        for (uintptr_t page = addr; page < addr + static_cast<uintptr_t>(length);
             page += page_size) {
            uintptr_t tmp_pa = 0U;
            HAL::PageFlags tmp_fl = HAL::PageFlags::None;
            uint64_t tmp_pte = 0ULL;
            if (proc->address_space().query_user_page(page, tmp_pa, tmp_fl) ||
                proc->address_space().query_pte_flags(page, tmp_pte)) {
                hint_free = false;
                break;
            }
        }
        if (hint_free) {
            map_start = addr;
            map_end = addr + static_cast<uintptr_t>(length);
        }
    }

    if (map_start == 0U) {
        const uintptr_t arena_top = proc->mmap_next_vaddr();
        if (length <= arena_top &&
            (arena_top - static_cast<uintptr_t>(length)) >= MMAP_ARENA_FLOOR) {
            const uintptr_t candidate_start = arena_top - static_cast<uintptr_t>(length);
            if (candidate_start > proc->brk_current()) {
                map_start = candidate_start;
                map_end = arena_top;
                used_arena_bump = true;
            }
        }
    }

    if (map_start == 0U && va_limit > HIGH_MMAP_ARENA_FLOOR &&
        length <= (va_limit - HIGH_MMAP_ARENA_FLOOR)) {
        const uintptr_t max_start = va_limit - static_cast<uintptr_t>(length);
        for (uintptr_t cand = HIGH_MMAP_ARENA_FLOOR; cand <= max_start;
             cand += HIGH_MMAP_SEARCH_STEP) {
            bool range_free = true;
            for (uintptr_t probe = cand; probe < cand + static_cast<uintptr_t>(length);
                 probe += page_size) {
                uintptr_t tmp_pa = 0U;
                HAL::PageFlags tmp_fl = HAL::PageFlags::None;
                uint64_t tmp_pte = 0ULL;
                if (proc->address_space().query_user_page(probe, tmp_pa, tmp_fl) ||
                    proc->address_space().query_pte_flags(probe, tmp_pte)) {
                    range_free = false;
                    cand = probe & ~(HIGH_MMAP_SEARCH_STEP - 1U);
                    break;
                }
            }
            if (range_free) {
                map_start = cand;
                map_end = cand + static_cast<uintptr_t>(length);
                break;
            }
        }
    }

    if (map_start == 0U) {
        return ERR_ENOMEM;
    }

    HAL::PageFlags page_flags = HAL::PageFlags::None;
    if (prot != PROT_NONE) {
        page_flags = HAL::PageFlags::User;
        if ((prot & PROT_READ) != 0U) {
            page_flags |= HAL::PageFlags::Read;
        }
        if ((prot & PROT_WRITE) != 0U) {
            page_flags |= HAL::PageFlags::Write;
        }
        if ((prot & PROT_EXEC) != 0U) {
            page_flags |= HAL::PageFlags::Execute;
        }
    }

    for (uintptr_t page_vaddr = map_start; page_vaddr < map_end; page_vaddr += page_size) {
        if (!proc->address_space().map_demand_zero(page_vaddr, page_flags)) {
            unmap_and_free_range(proc->address_space(), map_start, page_vaddr, page_size);
            return ERR_ENOMEM;
        }
    }

    if (used_arena_bump) {
        proc->set_mmap_next_vaddr(map_start);
    }
    return static_cast<int64_t>(map_start);
}

int64_t sys_munmap(uintptr_t addr, uint64_t length) noexcept {
    constexpr uintptr_t HIGH_MMAP_ARENA_FLOOR = 0x0000000080000000ULL;

    auto* const proc = Kernel::Process::Process::current();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const auto platform = HAL::get_platform_info();
    const size_t page_size = platform.page_size;
    const uintptr_t page_mask = page_size - 1U;
    const uintptr_t va_limit = Kernel::MM::user_addr_space_limit();
    const uintptr_t stack_floor =
        platform.user_stack_top - Kernel::Process::DEFAULT_USER_STACK_SIZE;

    if ((addr & page_mask) != 0U || length == 0U || (length & page_mask) != 0U) {
        return ERR_EINVAL;
    }
    if (addr >= va_limit || length > (va_limit - addr)) {
        return ERR_EINVAL;
    }

    const uintptr_t end_addr = addr + static_cast<uintptr_t>(length);
    const bool in_lower_arena = (addr >= MMAP_ARENA_FLOOR && end_addr <= stack_floor);
    const bool in_high_arena = (addr >= HIGH_MMAP_ARENA_FLOOR && end_addr <= va_limit);
    if (!in_lower_arena && !in_high_arena) {
        return ERR_EINVAL;
    }

    unmap_and_free_range(proc->address_space(), addr, end_addr, page_size);

    if (in_lower_arena && addr == proc->mmap_next_vaddr()) {
        proc->set_mmap_next_vaddr(end_addr);
    }

    return 0;
}

int64_t sys_mprotect(uintptr_t addr, uint64_t length, uint64_t prot) noexcept {
    auto* const proc = Kernel::Process::Process::current();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uintptr_t page_mask = page_size - 1U;
    const uintptr_t va_limit = Kernel::MM::user_addr_space_limit();

    if ((addr & page_mask) != 0U) {
        return ERR_EINVAL;
    }
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0U) {
        return ERR_EINVAL;
    }
    if (addr >= va_limit) {
        return ERR_ENOMEM;
    }
    if (length == 0U) {
        return 0;
    }
    if (length > (UINT64_MAX - page_mask)) {
        return ERR_ENOMEM;
    }

    const uint64_t aligned_len = (length + page_mask) & ~page_mask;
    if (aligned_len > (va_limit - addr)) {
        return ERR_ENOMEM;
    }

    HAL::PageFlags page_flags = HAL::PageFlags::None;
    if (prot != PROT_NONE) {
        page_flags = HAL::PageFlags::User;
        if ((prot & PROT_READ) != 0U) {
            page_flags |= HAL::PageFlags::Read;
        }
        if ((prot & PROT_WRITE) != 0U) {
            page_flags |= HAL::PageFlags::Write;
        }
        if ((prot & PROT_EXEC) != 0U) {
            page_flags |= HAL::PageFlags::Execute;
        }
    }

    const uintptr_t end_addr = addr + static_cast<uintptr_t>(aligned_len);
    for (uintptr_t page_vaddr = addr; page_vaddr < end_addr; page_vaddr += page_size) {
        if (!proc->address_space().protect_user_page(page_vaddr, page_flags)) {
            return ERR_ENOMEM;
        }
    }

    if ((prot & PROT_EXEC) != 0U) {
        HAL::get_cpu().invalidate_icache();
    }

    return 0;
}

} // namespace Kernel::Syscalls
