// File: kernel/process/stack_builder.cpp
#include "process/stack_builder.hpp"

#include "hal/cpu.hpp"
#include "mm/pfa.hpp"

extern "C" void* memcpy(void* dest, const void* src, size_t count) noexcept;

namespace {

size_t str_len(const char* str) noexcept {
    size_t len = 0U;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

void generate_at_random_bytes(uint8_t out[16]) noexcept {
    static uint64_t seed = 0x9E3779B97F4A7C15ULL;
    seed ^= (seed << 13U);
    seed ^= (seed >> 7U);
    seed ^= (seed << 17U);
    const uint64_t word0 = seed;

    seed += 0xBF58476D1CE4E5B9ULL;
    seed ^= (seed << 13U);
    seed ^= (seed >> 7U);
    seed ^= (seed << 17U);
    const uint64_t word1 = seed;

    memcpy(out, &word0, sizeof(word0));
    memcpy(out + 8U, &word1, sizeof(word1));
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

size_t populate_standard_auxv(const Loader::LoadedElf& loaded, Elf64_auxv_t* out_auxv,
                              size_t capacity) noexcept {
    if (out_auxv == nullptr || capacity < STANDARD_AUXV_COUNT) {
        return 0U;
    }

    const uint64_t page_size = HAL::get_platform_info().page_size;
    size_t idx = 0U;
    out_auxv[idx++] = Elf64_auxv_t{AT_PAGESZ, {page_size}};
    out_auxv[idx++] = Elf64_auxv_t{AT_PHDR, {loaded.phdr_vaddr}};
    out_auxv[idx++] = Elf64_auxv_t{AT_PHENT, {loaded.phentsize}};
    out_auxv[idx++] = Elf64_auxv_t{AT_PHNUM, {loaded.phnum}};
    out_auxv[idx++] = Elf64_auxv_t{AT_ENTRY, {loaded.entry_point}};
    out_auxv[idx++] = Elf64_auxv_t{AT_UID, {0ULL}};
    out_auxv[idx++] = Elf64_auxv_t{AT_EUID, {0ULL}};
    out_auxv[idx++] = Elf64_auxv_t{AT_GID, {0ULL}};
    out_auxv[idx++] = Elf64_auxv_t{AT_EGID, {0ULL}};
    out_auxv[idx++] = Elf64_auxv_t{AT_SECURE, {0ULL}};
    out_auxv[idx++] = Elf64_auxv_t{AT_RANDOM, {0ULL}}; // Patched by build_abi_layout()
    return idx;
}

Result<UserStack, StackBuilderError>
UserStackBuilder::create_and_map(MM::AddressSpace& address_space, uintptr_t virt_top,
                                 size_t stack_size) noexcept {
    const size_t page_size = HAL::get_platform_info().page_size;

    if (stack_size == 0U || (stack_size % page_size) != 0U) {
        return StackBuilderError::InvalidAlignment;
    }

    const size_t num_pages = stack_size / page_size;
    const uintptr_t phys_base = alloc_frames(num_pages);
    if (phys_base == 0U) {
        return StackBuilderError::AllocationFailed;
    }

    const uintptr_t virt_base = virt_top - stack_size;

    auto* const kptr = reinterpret_cast<uint8_t*>(MM::phys_to_virt(phys_base));
    for (size_t i = 0U; i < stack_size; ++i) {
        kptr[i] = 0U;
    }

    const HAL::PageFlags flags =
        HAL::PageFlags::User | HAL::PageFlags::Read | HAL::PageFlags::Write;

    for (size_t i = 0U; i < num_pages; ++i) {
        const uintptr_t page_vaddr = virt_base + (i * page_size);
        const uintptr_t page_paddr = phys_base + (i * page_size);

        if (!address_space.map_user_page(page_vaddr, page_paddr, flags)) {
            free_frames(phys_base, num_pages);
            return StackBuilderError::MappingFailed;
        }
    }

    HAL::get_cpu().flush_dcache_range(reinterpret_cast<uintptr_t>(kptr), stack_size);

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

    constexpr size_t MAX_ABI_ENTRIES = 64U;

    size_t envc = 0U;
    if (envp != nullptr) {
        while (envp[envc] != nullptr) {
            envc++;
        }
    }

    if (static_cast<size_t>(argc) > MAX_ABI_ENTRIES || envc > MAX_ABI_ENTRIES) {
        return StackBuilderError::BufferOverflow;
    }

    auto* const kbuf = reinterpret_cast<uint8_t*>(MM::phys_to_virt(stack.phys_base));
    uintptr_t curr_vaddr = stack.virt_top;

    auto check_overflow = [&](size_t bytes) -> bool {
        return (curr_vaddr >= stack.virt_base + bytes);
    };

    auto push_bytes = [&](const void* data, size_t len) -> bool {
        if (data == nullptr || len == 0U) {
            return true;
        }
        if (!check_overflow(len)) {
            return false;
        }
        curr_vaddr -= len;
        uint8_t* dest = kbuf + (curr_vaddr - stack.virt_base);
        memcpy(dest, data, len);
        return true;
    };

    auto push_string = [&](const char* str, uintptr_t& out_vaddr) -> bool {
        if (str == nullptr) {
            return false;
        }
        const size_t len = str_len(str) + 1U;
        if (!check_overflow(len)) {
            return false;
        }
        curr_vaddr -= len;
        uint8_t* dest = kbuf + (curr_vaddr - stack.virt_base);
        memcpy(dest, str, len);
        out_vaddr = curr_vaddr;
        return true;
    };

    // 1. Push envp string payloads
    uintptr_t envp_vaddrs[MAX_ABI_ENTRIES] = {0U};
    for (size_t i = 0U; i < envc; ++i) {
        if (!push_string(envp[i], envp_vaddrs[i])) {
            return StackBuilderError::BufferOverflow;
        }
    }

    // 2. Push argv string payloads
    uintptr_t argv_vaddrs[MAX_ABI_ENTRIES] = {0U};
    for (int i = 0; i < argc; ++i) {
        if (!push_string(argv[i], argv_vaddrs[i])) {
            return StackBuilderError::BufferOverflow;
        }
    }

    // 3. Push 16 random bytes for AT_RANDOM
    uint8_t random_bytes[16]{};
    generate_at_random_bytes(random_bytes);
    if (!push_bytes(random_bytes, sizeof(random_bytes))) {
        return StackBuilderError::BufferOverflow;
    }
    const uintptr_t at_random_vaddr = curr_vaddr;

    // 4. Align downward to 16 bytes, then add 8 bytes padding if total pointer/header words is odd
    // so that final SP_EL0 is 16-byte aligned with argc at [SP+0] and argv[0] at [SP+8].
    curr_vaddr &= ~0xFULL;
    const size_t total_words =
        1U + (static_cast<size_t>(argc) + 1U) + (envc + 1U) + ((auxv_count + 1U) * 2U);
    if ((total_words & 1U) != 0U) {
        if (!check_overflow(sizeof(uint64_t))) {
            return StackBuilderError::BufferOverflow;
        }
        curr_vaddr -= sizeof(uint64_t);
    }

    // 5. Push Auxiliary Vector (auxv) terminating with AT_NULL
    const Elf64_auxv_t null_aux{AT_NULL, {0ULL}};
    if (!push_bytes(&null_aux, sizeof(null_aux))) {
        return StackBuilderError::BufferOverflow;
    }

    if (auxv != nullptr && auxv_count > 0U) {
        for (size_t i = auxv_count; i > 0U; --i) {
            Elf64_auxv_t entry = auxv[i - 1U];
            if (entry.a_type == AT_RANDOM && entry.a_un.a_val == 0ULL) {
                entry.a_un.a_val = at_random_vaddr;
            }
            if (!push_bytes(&entry, sizeof(Elf64_auxv_t))) {
                return StackBuilderError::BufferOverflow;
            }
        }
    }

    // 6. Push envp pointer array (NULL terminated)
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

    // 7. Push argv pointer array (NULL terminated)
    if (!push_bytes(&null_ptr, sizeof(null_ptr))) {
        return StackBuilderError::BufferOverflow;
    }
    for (int i = argc; i > 0; --i) {
        const uint64_t ptr = argv_vaddrs[i - 1];
        if (!push_bytes(&ptr, sizeof(ptr))) {
            return StackBuilderError::BufferOverflow;
        }
    }

    stack.argv_vaddr = curr_vaddr;

    // 8. Push argc at the exact 16-byte aligned SP_EL0
    const uint64_t argc_val = static_cast<uint64_t>(argc);
    if (!push_bytes(&argc_val, sizeof(argc_val))) {
        return StackBuilderError::BufferOverflow;
    }
    stack.argc = argc_val;
    stack.final_sp_el0 = curr_vaddr;

    HAL::get_cpu().flush_dcache_range(reinterpret_cast<uintptr_t>(kbuf), stack.size);
    return stack.final_sp_el0;
}

} // namespace Kernel::Process