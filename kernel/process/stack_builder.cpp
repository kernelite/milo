#include "process/stack_builder.hpp"

#include "../arch/memory_config.hpp"
#include "mm/pfa.hpp"

namespace {

inline void flush_dcache_range(uintptr_t addr, size_t size) noexcept {
    constexpr uintptr_t line_size = 64U;
    const uintptr_t end = addr + size;
    addr &= ~(line_size - 1U);
    for (; addr < end; addr += line_size) {
        asm volatile("dc civac, %0" ::"r"(addr) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

size_t str_len(const char* str) noexcept {
    size_t len = 0U;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

} // namespace

namespace Kernel::Process {

const char* stack_builder_error_str(StackBuilderError error) noexcept {
    switch (error) {
    case StackBuilderError::Success:
        return "Success";
    case StackBuilderError::AllocationFailed:
        return "AllocationFailed";
    case StackBuilderError::MappingFailed:
        return "MappingFailed";
    case StackBuilderError::BufferOverflow:
        return "BufferOverflow";
    case StackBuilderError::InvalidAlignment:
        return "InvalidAlignment";
    case StackBuilderError::NullArgument:
        return "NullArgument";
    default:
        return "UnknownError";
    }
}

Result<UserStack, StackBuilderError>
UserStackBuilder::create_and_map(MM::AddressSpace& address_space, uintptr_t virt_top,
                                 size_t stack_size) noexcept {
    if (stack_size == 0U || (stack_size % Arch::PAGE_SIZE) != 0U) {
        return StackBuilderError::InvalidAlignment;
    }

    const size_t num_pages = stack_size / Arch::PAGE_SIZE;
    const uintptr_t phys_base = alloc_frames(num_pages);
    if (phys_base == 0U) {
        return StackBuilderError::AllocationFailed;
    }

    const uintptr_t virt_base = virt_top - stack_size;

    // Zero allocated physical stack frames via linear higher-half map
    auto* const kptr = reinterpret_cast<uint8_t*>(MM::phys_to_virt(phys_base));
    for (size_t i = 0U; i < stack_size; ++i) {
        kptr[i] = 0U;
    }

    // Map stack pages: User | Read | Write (Execute omitted -> UXN bit enforced)
    const HAL::PageFlags flags =
        HAL::PageFlags::User | HAL::PageFlags::Read | HAL::PageFlags::Write;

    for (size_t i = 0U; i < num_pages; ++i) {
        const uintptr_t page_vaddr = virt_base + (i * Arch::PAGE_SIZE);
        const uintptr_t page_paddr = phys_base + (i * Arch::PAGE_SIZE);

        if (!address_space.map_user_page(page_vaddr, page_paddr, flags)) {
            free_frames(phys_base, num_pages);
            return StackBuilderError::MappingFailed;
        }
    }

    flush_dcache_range(reinterpret_cast<uintptr_t>(kptr), stack_size);

    UserStack stack{};
    stack.phys_base = phys_base;
    stack.size = stack_size;
    stack.num_pages = num_pages;
    stack.virt_top = virt_top;
    stack.virt_base = virt_base;
    return stack;
}

Result<uintptr_t, StackBuilderError>
UserStackBuilder::build_abi_layout(UserStack& stack, int argc, const char* const* argv,
                                   const char* const* envp, const Elf64_auxv_t* auxv,
                                   size_t auxv_count) noexcept {
    if (stack.phys_base == 0U || stack.virt_top == 0U || stack.size == 0U) {
        return StackBuilderError::NullArgument;
    }
    if (argc < 0 || (argc > 0 && argv == nullptr)) {
        return StackBuilderError::NullArgument;
    }

    auto* const kbuf = reinterpret_cast<uint8_t*>(MM::phys_to_virt(stack.phys_base));
    uintptr_t curr_vaddr = stack.virt_top;

    auto check_overflow = [&](size_t bytes) -> bool {
        return (curr_vaddr >= stack.virt_base + bytes);
    };

    auto push_bytes = [&](const void* data, size_t len) -> bool {
        if (!check_overflow(len)) {
            return false;
        }
        curr_vaddr -= len;
        uint8_t* dest = kbuf + (curr_vaddr - stack.virt_base);
        for (size_t i = 0U; i < len; ++i) {
            dest[i] = static_cast<const uint8_t*>(data)[i];
        }
        return true;
    };

    auto push_string = [&](const char* str, uintptr_t& out_vaddr) -> bool {
        const size_t len = str_len(str) + 1U;
        if (!check_overflow(len)) {
            return false;
        }
        curr_vaddr -= len;
        uint8_t* dest = kbuf + (curr_vaddr - stack.virt_base);
        for (size_t i = 0U; i < len; ++i) {
            dest[i] = static_cast<uint8_t>(str[i]);
        }
        out_vaddr = curr_vaddr;
        return true;
    };

    // 1. Push envp string payloads
    size_t envc = 0U;
    if (envp != nullptr) {
        while (envp[envc] != nullptr) {
            envc++;
        }
    }

    uintptr_t envp_vaddrs[64] = {0U};
    for (size_t i = 0U; i < envc && i < 64U; ++i) {
        if (!push_string(envp[i], envp_vaddrs[i])) {
            return StackBuilderError::BufferOverflow;
        }
    }

    // 1b. Push argv string payloads
    uintptr_t argv_vaddrs[64] = {0U};
    for (int i = 0; i < argc && i < 64; ++i) {
        if (!push_string(argv[i], argv_vaddrs[i])) {
            return StackBuilderError::BufferOverflow;
        }
    }

    // 2. Push Auxiliary Vector (auxv) terminating with AT_NULL
    const Elf64_auxv_t null_aux{PT_NULL, {0ULL}};
    if (!push_bytes(&null_aux, sizeof(null_aux))) {
        return StackBuilderError::BufferOverflow;
    }

    if (auxv != nullptr && auxv_count > 0U) {
        for (size_t i = auxv_count; i > 0U; --i) {
            if (!push_bytes(&auxv[i - 1U], sizeof(Elf64_auxv_t))) {
                return StackBuilderError::BufferOverflow;
            }
        }
    }

    // 3. Push envp pointer array (NULL terminated)
    const uint64_t null_ptr = 0ULL;
    if (!push_bytes(&null_ptr, sizeof(null_ptr))) {
        return StackBuilderError::BufferOverflow;
    }
    for (size_t i = envc; i > 0U; --i) {
        const uint64_t ptr = envp_vaddrs[i - 1U];
        if (!push_bytes(&ptr, sizeof(ptr))) {
            return StackBuilderError::BufferOverflow;
        }
    }

    // 4. Push argv pointer array (NULL terminated)
    if (!push_bytes(&null_ptr, sizeof(null_ptr))) {
        return StackBuilderError::BufferOverflow;
    }
    for (int i = argc; i > 0; --i) {
        const uint64_t ptr = argv_vaddrs[i - 1];
        if (!push_bytes(&ptr, sizeof(ptr))) {
            return StackBuilderError::BufferOverflow;
        }
    }

    // Capture user-space address of argv[0] pointer slot
    stack.argv_vaddr = curr_vaddr;

    // 5. Push argc
    const uint64_t argc_val = static_cast<uint64_t>(argc);
    if (!push_bytes(&argc_val, sizeof(argc_val))) {
        return StackBuilderError::BufferOverflow;
    }
    stack.argc = argc_val;

    // 6. Align Stack Pointer downwards to 16-byte boundary (AAPCS64 requirement)
    curr_vaddr &= ~0xFULL;
    stack.final_sp_el0 = curr_vaddr;

    flush_dcache_range(reinterpret_cast<uintptr_t>(kbuf), stack.size);
    return stack.final_sp_el0;
}

} // namespace Kernel::Process