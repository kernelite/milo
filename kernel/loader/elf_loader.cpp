// File: kernel/loader/elf_loader.cpp
#include "loader/elf_loader.hpp"

#include "hal/cpu.hpp"
#include "mm/pfa.hpp"

namespace {

constexpr size_t MAX_TRACKED_SEGMENTS = 32U;
constexpr uintptr_t PIE_DEFAULT_LOAD_BIAS = 0x0000000000400000ULL;

struct SegmentAllocationRecord {
    uintptr_t paddr_base{0U};
    size_t num_pages{0U};
};

constexpr HAL::PageFlags translate_segment_flags(uint32_t p_flags) noexcept {
    HAL::PageFlags flags = HAL::PageFlags::User;
    if ((p_flags & PF_R) != 0U) {
        flags = flags | HAL::PageFlags::Read;
    }
    if ((p_flags & PF_W) != 0U) {
        flags = flags | HAL::PageFlags::Write;
    }
    if ((p_flags & PF_X) != 0U) {
        flags = flags | HAL::PageFlags::Execute;
    }
    return flags;
}

} // namespace

namespace Kernel::Loader {

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

    const size_t page_size = HAL::get_platform_info().page_size;
    const uintptr_t load_bias = elf.is_position_independent() ? PIE_DEFAULT_LOAD_BIAS : 0U;

    SegmentAllocationRecord tracked_allocations[MAX_TRACKED_SEGMENTS];
    size_t tracked_count = 0U;
    uintptr_t max_vaddr = 0U;
    uintptr_t phdr_vaddr = 0U;

    const auto& ehdr = elf.header();
    elf.for_each_segment([&](const Elf64Segment& seg) -> bool {
        if (seg.type == PT_PHDR) {
            phdr_vaddr = static_cast<uintptr_t>(seg.vaddr) + load_bias;
            return false;
        }
        return true;
    });

    if (phdr_vaddr == 0U) {
        elf.for_each_load_segment([&](const Elf64Segment& seg) -> bool {
            if (ehdr.e_phoff >= seg.offset && ehdr.e_phoff < (seg.offset + seg.filesz)) {
                phdr_vaddr = static_cast<uintptr_t>(seg.vaddr) + load_bias +
                             static_cast<uintptr_t>(ehdr.e_phoff - seg.offset);
                return false;
            }
            return true;
        });
    }

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

        if (seg.vaddr > UINT64_MAX - load_bias ||
            (seg.vaddr + load_bias) > UINT64_MAX - seg.memsz) {
            pipeline_failed = true;
            failure_reason = ElfLoaderError::ArithmeticOverflow;
            return false;
        }

        const uint64_t seg_vaddr = seg.vaddr + load_bias;
        const uint64_t page_mask = page_size - 1U;
        const uint64_t start_page = seg_vaddr & ~page_mask;
        const uint64_t end_page = (seg_vaddr + seg.memsz + page_mask) & ~page_mask;
        const size_t num_pages = static_cast<size_t>((end_page - start_page) / page_size);

        if (end_page > max_vaddr) {
            max_vaddr = static_cast<uintptr_t>(end_page);
        }

        if (tracked_count >= MAX_TRACKED_SEGMENTS) {
            pipeline_failed = true;
            failure_reason = ElfLoaderError::OutOfMemory;
            return false;
        }

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

        const uintptr_t higher_half_vaddr = MM::phys_to_virt(paddr_base);
        auto* const dest_base = reinterpret_cast<uint8_t*>(higher_half_vaddr);

        for (size_t i = 0U; i < num_pages * page_size; ++i) {
            dest_base[i] = 0U;
        }

        const uint64_t page_offset = seg_vaddr & page_mask;
        uint8_t* const payload_dest = dest_base + page_offset;

        if (seg.filesz > 0U && seg.data != nullptr) {
            for (size_t i = 0U; i < seg.filesz; ++i) {
                payload_dest[i] = seg.data[i];
            }
        }

        if (seg.memsz > seg.filesz) {
            uint8_t* const bss_dest = payload_dest + seg.filesz;
            const size_t bss_len = static_cast<size_t>(seg.memsz - seg.filesz);
            for (size_t i = 0U; i < bss_len; ++i) {
                bss_dest[i] = 0U;
            }
        }

        HAL::get_cpu().flush_dcache_range(higher_half_vaddr, num_pages * page_size);

        if ((seg.flags & PF_X) != 0U) {
            HAL::get_cpu().invalidate_icache();
        }

        const HAL::PageFlags page_flags = translate_segment_flags(seg.flags);
        for (size_t i = 0U; i < num_pages; ++i) {
            const uintptr_t page_vaddr = start_page + (i * page_size);
            const uintptr_t page_paddr = paddr_base + (i * page_size);

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
        .entry_point = static_cast<uintptr_t>(elf.entry_point()) + load_bias,
        .max_vaddr = max_vaddr,
        .phdr_vaddr = phdr_vaddr,
        .phentsize = ehdr.e_phentsize,
        .phnum = ehdr.e_phnum,
        .loaded_segment_count = tracked_count,
    };
}

} // namespace Kernel::Loader