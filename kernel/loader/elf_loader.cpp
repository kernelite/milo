#include "loader/elf_loader.hpp"

#include "../../arch/memory_config.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Loader {

constexpr size_t MAX_TRACKED_SEGMENTS = 32U;

struct SegmentAllocationRecord {
    uintptr_t paddr_base{0U};
    size_t num_pages{0U};
};

inline void flush_dcache_range(uintptr_t addr, size_t size) noexcept {
    constexpr uintptr_t line_size = 64U;
    const uintptr_t end = addr + size;
    addr &= ~(line_size - 1U);
    for (; addr < end; addr += line_size) {
        asm volatile("dc civac, %0" ::"r"(addr) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

constexpr HAL::PageFlags translate_segment_flags(uint32_t pf_flags) noexcept {
    HAL::PageFlags flags = HAL::PageFlags::User;
    if ((pf_flags & PF_R) != 0U) {
        flags |= HAL::PageFlags::Read;
    }
    if ((pf_flags & PF_W) != 0U) {
        flags |= HAL::PageFlags::Write;
    }
    if ((pf_flags & PF_X) != 0U) {
        flags |= HAL::PageFlags::Execute;
    }
    return flags;
}

const char* elf_loader_error_str(ElfLoaderError error) noexcept {
    switch (error) {
    case ElfLoaderError::Success:
        return "Success";
    case ElfLoaderError::InvalidElf:
        return "InvalidElf";
    case ElfLoaderError::NoLoadSegments:
        return "NoLoadSegments";
    case ElfLoaderError::OutOfMemory:
        return "OutOfMemory";
    case ElfLoaderError::PageMappingFailed:
        return "PageMappingFailed";
    case ElfLoaderError::InvalidSegmentAlignment:
        return "InvalidSegmentAlignment";
    case ElfLoaderError::ArithmeticOverflow:
        return "ArithmeticOverflow";
    default:
        return "UnknownError";
    }
}

Result<LoadedElf, ElfLoaderError> load_elf(const uint8_t* elf_data, size_t elf_size) noexcept {
    const auto parse_res = Elf64View::parse(elf_data, elf_size);
    if (!parse_res) {
        return ElfLoaderError::InvalidElf;
    }

    const auto& elf = parse_res.value();
    if (elf.load_segment_count() == 0U) {
        return ElfLoaderError::NoLoadSegments;
    }

    auto as_res = MM::AddressSpace::create();
    if (!as_res) {
        return ElfLoaderError::OutOfMemory;
    }
    MM::AddressSpace address_space = static_cast<MM::AddressSpace&&>(as_res.value());

    SegmentAllocationRecord tracked_allocations[MAX_TRACKED_SEGMENTS];
    size_t tracked_count = 0U;

    auto rollback_and_abort = [&](ElfLoaderError err) -> Result<LoadedElf, ElfLoaderError> {
        for (size_t i = 0U; i < tracked_count; ++i) {
            free_frames(tracked_allocations[i].paddr_base, tracked_allocations[i].num_pages);
        }
        address_space.destroy();
        return err;
    };

    bool pipeline_failed = false;
    ElfLoaderError failure_reason = ElfLoaderError::Success;

    elf.for_each_load_segment([&](const Elf64Segment& seg) -> bool {
        if (seg.memsz == 0U) {
            return true;
        }

        // Arithmetic overflow check on segment boundaries
        if (seg.vaddr > UINT64_MAX - seg.memsz) {
            pipeline_failed = true;
            failure_reason = ElfLoaderError::ArithmeticOverflow;
            return false;
        }

        // Step 1: Calculate Page Boundaries
        constexpr uint64_t page_mask = Arch::PAGE_SIZE - 1U;
        const uint64_t start_page = seg.vaddr & ~page_mask;
        const uint64_t end_page = (seg.vaddr + seg.memsz + page_mask) & ~page_mask;
        const size_t num_pages = static_cast<size_t>((end_page - start_page) / Arch::PAGE_SIZE);

        if (tracked_count >= MAX_TRACKED_SEGMENTS) {
            pipeline_failed = true;
            failure_reason = ElfLoaderError::OutOfMemory;
            return false;
        }

        // Step 2: Physical Frame Allocation & Error Handling
        const uintptr_t paddr_base = alloc_frames(num_pages);
        if (paddr_base == 0U) {
            pipeline_failed = true;
            failure_reason = ElfLoaderError::OutOfMemory;
            return false;
        }

        tracked_allocations[tracked_count++] = SegmentAllocationRecord{
            .paddr_base = paddr_base,
            .num_pages = num_pages,
        };

        // Step 3: Payload Copy & Zeroing (BSS) via Higher-Half Linear Map
        const uintptr_t higher_half_vaddr = MM::phys_to_virt(paddr_base);
        auto* const dest_base = reinterpret_cast<uint8_t*>(higher_half_vaddr);

        // Explicitly zero the full span of allocated frames to prevent memory leaks
        for (size_t i = 0U; i < num_pages * Arch::PAGE_SIZE; ++i) {
            dest_base[i] = 0U;
        }

        const uint64_t page_offset = seg.vaddr & page_mask;
        uint8_t* const payload_dest = dest_base + page_offset;

        // Copy file content if present
        if (seg.filesz > 0U && seg.data != nullptr) {
            for (size_t i = 0U; i < seg.filesz; ++i) {
                payload_dest[i] = seg.data[i];
            }
        }

        // Zero-fill trailing BSS bytes: (p_memsz - p_filesz)
        if (seg.memsz > seg.filesz) {
            uint8_t* const bss_dest = payload_dest + seg.filesz;
            const size_t bss_len = static_cast<size_t>(seg.memsz - seg.filesz);
            for (size_t i = 0U; i < bss_len; ++i) {
                bss_dest[i] = 0U;
            }
        }

        flush_dcache_range(higher_half_vaddr, num_pages * Arch::PAGE_SIZE);

        // Instruction cache invalidation if segment is executable
        if ((seg.flags & PF_X) != 0U) {
            asm volatile("dsb ish\n\t"
                         "ic ialluis\n\t"
                         "dsb ish\n\t"
                         "isb" ::
                             : "memory");
        }

        // Step 4: Page Table Mapping
        const HAL::PageFlags page_flags = translate_segment_flags(seg.flags);
        for (size_t i = 0U; i < num_pages; ++i) {
            const uintptr_t page_vaddr = start_page + (i * Arch::PAGE_SIZE);
            const uintptr_t page_paddr = paddr_base + (i * Arch::PAGE_SIZE);

            if (!address_space.map_user_page(page_vaddr, page_paddr, page_flags)) {
                pipeline_failed = true;
                failure_reason = ElfLoaderError::PageMappingFailed;
                return false;
            }
        }

        return true;
    });

    if (pipeline_failed) {
        return rollback_and_abort(failure_reason);
    }

    return LoadedElf{
        .address_space = static_cast<MM::AddressSpace&&>(address_space),
        .entry_point = elf.entry_point(),
        .loaded_segment_count = tracked_count,
    };
}

} // namespace Kernel::Loader