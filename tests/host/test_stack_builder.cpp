#include "mm/pfa.hpp"
#include "mocks/host_hal.hpp"
#include "process/stack_builder.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

TEST_CASE("UserStackBuilder", "System V ABI Layout Construction & Auxiliary Vector") {
    pfa_init(HostHAL::SIMULATED_RAM_BASE, HostHAL::SIMULATED_RAM_SIZE);
    const size_t baseline_pages = pfa_allocated_pages_count();

    auto address_space_res = Kernel::MM::AddressSpace::create();
    EXPECT_TRUE(address_space_res.is_ok());
    auto address_space = static_cast<Kernel::MM::AddressSpace&&>(address_space_res.value());

    auto stack_res = Kernel::Process::UserStackBuilder::create_and_map(address_space);
    EXPECT_TRUE(stack_res.is_ok());
    auto user_stack = stack_res.value();

    const char* const test_argv[] = {"milo_init", "hello_aarch64", "system_v_abi", "test_arg"};
    const char* const test_envp[] = {"PATH=/usr/bin:/bin", "USER=milo", "TERM=vt100", nullptr};

    Kernel::Loader::LoadedElf loaded_elf{};
    loaded_elf.entry_point = 0x00400080ULL;
    loaded_elf.phdr_vaddr = 0x00400040ULL;
    loaded_elf.phentsize = sizeof(Elf64_Phdr);
    loaded_elf.phnum = 2U;

    Elf64_auxv_t auxv_entries[Kernel::Process::STANDARD_AUXV_COUNT]{};
    const size_t auxv_count = Kernel::Process::populate_standard_auxv(
        loaded_elf, auxv_entries, Kernel::Process::STANDARD_AUXV_COUNT);
    EXPECT_EQ(auxv_count, Kernel::Process::STANDARD_AUXV_COUNT);

    auto abi_res = Kernel::Process::UserStackBuilder::build_abi_layout(
        user_stack, 4, test_argv, test_envp, auxv_entries, auxv_count);
    EXPECT_TRUE(abi_res.is_ok());

    const uintptr_t final_sp_el0 = abi_res.value();
    EXPECT_EQ(final_sp_el0 % 16U, 0U);
    EXPECT_EQ(user_stack.argc, 4U);
    EXPECT_EQ(user_stack.argv_vaddr, final_sp_el0 + sizeof(uint64_t));

    const auto* stack_mem =
        reinterpret_cast<const uint8_t*>(Kernel::MM::phys_to_virt(user_stack.phys_base));
    const auto sp_offset = static_cast<size_t>(final_sp_el0 - user_stack.virt_base);

    // 1. Verify argc at [SP + 0]
    uint64_t stored_argc = 0ULL;
    std::memcpy(&stored_argc, stack_mem + sp_offset, sizeof(stored_argc));
    EXPECT_EQ(stored_argc, 4ULL);

    // 2. Verify argv[0..3] pointers and strings, plus argv[4] == NULL
    for (size_t arg_idx = 0U; arg_idx < 4U; ++arg_idx) {
        uint64_t arg_ptr = 0ULL;
        const size_t slot_offset = sp_offset + ((1U + arg_idx) * sizeof(uint64_t));
        std::memcpy(&arg_ptr, stack_mem + slot_offset, sizeof(arg_ptr));
        EXPECT_TRUE(arg_ptr >= user_stack.virt_base);
        EXPECT_TRUE(arg_ptr < user_stack.virt_top);

        const auto* arg_str = reinterpret_cast<const char*>(
            stack_mem + static_cast<size_t>(arg_ptr - user_stack.virt_base));
        EXPECT_EQ(std::strcmp(arg_str, test_argv[arg_idx]), 0);
    }
    uint64_t argv_terminator = 1ULL;
    std::memcpy(&argv_terminator, stack_mem + sp_offset + (5U * sizeof(uint64_t)),
                sizeof(argv_terminator));
    EXPECT_EQ(argv_terminator, 0ULL);

    // 3. Verify envp[0..2] pointers and strings, plus envp[3] == NULL
    const size_t envp_offset = sp_offset + (6U * sizeof(uint64_t));
    for (size_t env_idx = 0U; env_idx < 3U; ++env_idx) {
        uint64_t env_ptr = 0ULL;
        const size_t slot_offset = envp_offset + (env_idx * sizeof(uint64_t));
        std::memcpy(&env_ptr, stack_mem + slot_offset, sizeof(env_ptr));
        EXPECT_TRUE(env_ptr >= user_stack.virt_base);
        EXPECT_TRUE(env_ptr < user_stack.virt_top);

        const auto* env_str = reinterpret_cast<const char*>(
            stack_mem + static_cast<size_t>(env_ptr - user_stack.virt_base));
        EXPECT_EQ(std::strcmp(env_str, test_envp[env_idx]), 0);
    }
    uint64_t envp_terminator = 1ULL;
    std::memcpy(&envp_terminator, stack_mem + envp_offset + (3U * sizeof(uint64_t)),
                sizeof(envp_terminator));
    EXPECT_EQ(envp_terminator, 0ULL);

    // 4. Verify auxv entries and patched AT_RANDOM pointer
    const size_t auxv_offset = envp_offset + (4U * sizeof(uint64_t));
    bool found_at_random = false;
    for (size_t aux_idx = 0U; aux_idx < auxv_count; ++aux_idx) {
        Elf64_auxv_t aux_item{};
        std::memcpy(&aux_item, stack_mem + auxv_offset + (aux_idx * sizeof(Elf64_auxv_t)),
                    sizeof(aux_item));
        if (aux_item.a_type == AT_RANDOM) {
            found_at_random = true;
            EXPECT_TRUE(aux_item.a_un.a_val >= user_stack.virt_base);
            EXPECT_TRUE(aux_item.a_un.a_val < user_stack.virt_top);
        }
    }
    EXPECT_TRUE(found_at_random);

    free_frames(user_stack.phys_base, user_stack.num_pages);
    address_space.destroy();
    EXPECT_EQ(pfa_allocated_pages_count(), baseline_pages);
}

TEST_CASE("UserStackBuilder", "AAPCS64 16-Byte SP_EL0 Alignment Invariant") {
    HostHAL::reset();
    pfa_init(HostHAL::SIMULATED_RAM_BASE, HostHAL::SIMULATED_RAM_SIZE);

    const uintptr_t phys_buf = alloc_frames(4U);
    EXPECT_NE(phys_buf, 0U);

    Kernel::Process::UserStack stack{};
    stack.phys_base = phys_buf;
    stack.size = 4UL * 4096UL;
    stack.num_pages = 4U;
    stack.virt_top = 0x3FFF0000ULL;
    stack.virt_base = stack.virt_top - stack.size;

    const char* const argv_pool[] = {"a", "bb", "ccc", "dddd", "eeeee"};
    const char* const envp_pool[] = {"E1=1", "E2=22", "E3=333", "E4=4444", nullptr};
    const Elf64_auxv_t auxv_sample[] = {
        {AT_PAGESZ, {4096ULL}},
        {AT_UID, {0ULL}},
        {AT_RANDOM, {0ULL}},
    };

    for (int argc = 0; argc <= 5; ++argc) {
        for (size_t envc = 0U; envc <= 4U; ++envc) {
            std::vector<const char*> env_vec;
            for (size_t entry = 0U; entry < envc; ++entry) {
                env_vec.push_back(envp_pool[entry]);
            }
            env_vec.push_back(nullptr);

            // Without auxv
            const auto res_no_aux = Kernel::Process::UserStackBuilder::build_abi_layout(
                stack, argc, (argc > 0) ? argv_pool : nullptr, env_vec.data());
            EXPECT_TRUE(res_no_aux.is_ok());
            EXPECT_EQ(res_no_aux.value() % 16U, 0U);

            // With auxv
            const auto res_with_aux = Kernel::Process::UserStackBuilder::build_abi_layout(
                stack, argc, (argc > 0) ? argv_pool : nullptr, env_vec.data(), auxv_sample, 3U);
            EXPECT_TRUE(res_with_aux.is_ok());
            EXPECT_EQ(res_with_aux.value() % 16U, 0U);
        }
    }

    free_frames(phys_buf, 4U);
}

TEST_CASE("UserStackBuilder", "Overflow Protection & Null Argument Guards") {
    HostHAL::reset();
    pfa_init(HostHAL::SIMULATED_RAM_BASE, HostHAL::SIMULATED_RAM_SIZE);

    const uintptr_t phys_buf = alloc_frames(4U);
    EXPECT_NE(phys_buf, 0U);

    Kernel::Process::UserStack stack{};
    stack.phys_base = phys_buf;
    stack.size = 4UL * 4096UL;
    stack.num_pages = 4U;
    stack.virt_top = 0x3FFF0000ULL;
    stack.virt_base = stack.virt_top - stack.size;

    // 1. Null/invalid arguments
    Kernel::Process::UserStack empty_stack{};
    EXPECT_EQ(Kernel::Process::UserStackBuilder::build_abi_layout(empty_stack, 0, nullptr).error(),
              Kernel::Process::StackBuilderError::NullArgument);
    EXPECT_EQ(Kernel::Process::UserStackBuilder::build_abi_layout(stack, -1, nullptr).error(),
              Kernel::Process::StackBuilderError::NullArgument);
    EXPECT_EQ(Kernel::Process::UserStackBuilder::build_abi_layout(stack, 1, nullptr).error(),
              Kernel::Process::StackBuilderError::NullArgument);

    // 2. MAX_ABI_ENTRIES boundary (64 succeeds, 65 fails with BufferOverflow)
    std::vector<const char*> big_argv(65U, "arg");
    const auto res_64 =
        Kernel::Process::UserStackBuilder::build_abi_layout(stack, 64, big_argv.data());
    EXPECT_TRUE(res_64.is_ok());
    EXPECT_EQ(res_64.value() % 16U, 0U);

    const auto res_65 =
        Kernel::Process::UserStackBuilder::build_abi_layout(stack, 65, big_argv.data());
    EXPECT_TRUE(res_65.is_err());
    EXPECT_EQ(res_65.error(), Kernel::Process::StackBuilderError::BufferOverflow);

    // 3. envc > 64 fails with BufferOverflow
    std::vector<const char*> big_envp(66U, "K=V");
    big_envp[65] = nullptr;
    const auto res_env_65 = Kernel::Process::UserStackBuilder::build_abi_layout(
        stack, 1, big_argv.data(), big_envp.data());
    EXPECT_TRUE(res_env_65.is_err());
    EXPECT_EQ(res_env_65.error(), Kernel::Process::StackBuilderError::BufferOverflow);

    // 4. Payload exceeding stack buffer size fails with BufferOverflow
    std::vector<char> huge_str(stack.size, 'A');
    huge_str.back() = '\0';
    const char* huge_argv[] = {huge_str.data()};
    const auto res_str_ovf =
        Kernel::Process::UserStackBuilder::build_abi_layout(stack, 1, huge_argv);
    EXPECT_TRUE(res_str_ovf.is_err());
    EXPECT_EQ(res_str_ovf.error(), Kernel::Process::StackBuilderError::BufferOverflow);

    free_frames(phys_buf, 4U);
}