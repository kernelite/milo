#include "loader/elf64.hpp"
#include "loader/elf_loader.hpp"
#include "memory_config.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"
#include "mocks/host_hal.hpp"
#include "process/stack_builder.hpp"

#include <cstring>
#include <snitch/snitch.hpp>

namespace {

constexpr uintptr_t RAM_BASE = HostTest::SIM_RAM_BASE;
constexpr size_t RAM_SIZE = 4UL * 1024UL * 1024UL; // 4 MiB
constexpr uintptr_t KERNEL_START = RAM_BASE;
constexpr uintptr_t KERNEL_END = RAM_BASE + (64UL * 1024UL);

void init_stack_test_env() noexcept {
    HostTest::reset_host_hal();
    FDT::MemoryMap map{};
    map.regions[0] = FDT::MemoryRegion{.base_paddr = RAM_BASE, .size = RAM_SIZE};
    map.count = 1U;
    map.min_paddr = RAM_BASE;
    map.max_paddr = RAM_BASE + RAM_SIZE;
    map.total_usable_bytes = RAM_SIZE;
    pfa_init(map, KERNEL_START, KERNEL_END, 0U, 0U, 0U, 0U, nullptr);
}

uint64_t read_stack_u64(const Kernel::Process::UserStack& stack, uintptr_t vaddr) noexcept {
    const auto* const base =
        reinterpret_cast<const uint8_t*>(Kernel::MM::phys_to_virt(stack.phys_base));
    const size_t offset = static_cast<size_t>(vaddr - stack.virt_base);
    uint64_t val = 0ULL;
    std::memcpy(&val, base + offset, sizeof(val));
    return val;
}

const char* read_stack_cstr(const Kernel::Process::UserStack& stack, uintptr_t vaddr) noexcept {
    const auto* const base =
        reinterpret_cast<const char*>(Kernel::MM::phys_to_virt(stack.phys_base));
    const size_t offset = static_cast<size_t>(vaddr - stack.virt_base);
    return base + offset;
}

} // namespace

TEST_CASE("UserStackBuilder allocates, zeroes, and maps RW+NX user pages", "[stack][process]") {
    init_stack_test_env();
    const size_t baseline_free = pfa_free_pages_count();

    auto as_res = Kernel::MM::AddressSpace::create();
    REQUIRE(as_res.is_ok());
    auto address_space = static_cast<Kernel::MM::AddressSpace&&>(as_res.value());

    SECTION("Default 64 KiB stack maps 16 RW non-executable pages") {
        auto stack_res = Kernel::Process::UserStackBuilder::create_and_map(address_space);
        REQUIRE(stack_res.is_ok());

        const Kernel::Process::UserStack stack = stack_res.value();
        CHECK(stack.virt_top == Kernel::Process::DEFAULT_USER_STACK_TOP);
        CHECK(stack.size == Kernel::Process::DEFAULT_USER_STACK_SIZE);
        CHECK(stack.num_pages == 16U);
        CHECK(stack.virt_base == stack.virt_top - stack.size);
        CHECK(pfa_free_pages_count() == baseline_free - 16U);

        for (size_t page_idx = 0U; page_idx < stack.num_pages; ++page_idx) {
            const uintptr_t vaddr = stack.virt_base + (page_idx * Arch::PAGE_SIZE);
            uintptr_t mapped_paddr = 0U;
            HAL::PageFlags mapped_flags = HAL::PageFlags::None;

            REQUIRE(address_space.query_user_page(vaddr, mapped_paddr, mapped_flags));
            CHECK(mapped_paddr == stack.phys_base + (page_idx * Arch::PAGE_SIZE));
            CHECK(HAL::has_flag(mapped_flags, HAL::PageFlags::User));
            CHECK(HAL::has_flag(mapped_flags, HAL::PageFlags::Read));
            CHECK(HAL::has_flag(mapped_flags, HAL::PageFlags::Write));
            CHECK_FALSE(HAL::has_flag(mapped_flags, HAL::PageFlags::Execute));
        }

        // Backing physical frames must be zero-initialized
        const auto* const kptr =
            reinterpret_cast<const uint8_t*>(Kernel::MM::phys_to_virt(stack.phys_base));
        for (size_t idx = 0U; idx < stack.size; ++idx) {
            REQUIRE(kptr[idx] == 0U);
        }

        free_frames(stack.phys_base, stack.num_pages);
        address_space.destroy();
        CHECK(pfa_free_pages_count() == baseline_free);
    }

    SECTION("create_and_map rejects invalid alignment and rolls back on mapping failure") {
        auto bad_zero = Kernel::Process::UserStackBuilder::create_and_map(
            address_space, Kernel::Process::DEFAULT_USER_STACK_TOP, 0U);
        REQUIRE(bad_zero.is_err());
        CHECK(bad_zero.error() == Kernel::Process::StackBuilderError::InvalidAlignment);

        auto bad_unaligned = Kernel::Process::UserStackBuilder::create_and_map(
            address_space, Kernel::Process::DEFAULT_USER_STACK_TOP, Arch::PAGE_SIZE + 128U);
        REQUIRE(bad_unaligned.is_err());
        CHECK(bad_unaligned.error() == Kernel::Process::StackBuilderError::InvalidAlignment);

        // Allow 3 page mappings to succeed, fail on the 4th, and verify all 4 frames are freed
        HostTest::set_fail_map_after(3U);
        auto map_fail = Kernel::Process::UserStackBuilder::create_and_map(
            address_space, Kernel::Process::DEFAULT_USER_STACK_TOP, 4U * Arch::PAGE_SIZE);
        REQUIRE(map_fail.is_err());
        CHECK(map_fail.error() == Kernel::Process::StackBuilderError::MappingFailed);
        CHECK(pfa_free_pages_count() == baseline_free);
    }
}

TEST_CASE("UserStackBuilder constructs System V AArch64 ABI stack layout", "[stack][abi]") {
    init_stack_test_env();

    auto as_res = Kernel::MM::AddressSpace::create();
    REQUIRE(as_res.is_ok());
    auto address_space = static_cast<Kernel::MM::AddressSpace&&>(as_res.value());

    auto stack_res = Kernel::Process::UserStackBuilder::create_and_map(address_space);
    REQUIRE(stack_res.is_ok());
    Kernel::Process::UserStack stack = stack_res.value();

    SECTION("Full argc, argv, envp, and standard auxiliary vector layout") {
        Kernel::Loader::LoadedElf loaded{};
        loaded.entry_point = 0x00400120ULL;
        loaded.phdr_vaddr = 0x00400040ULL;
        loaded.phentsize = sizeof(Elf64_Phdr);
        loaded.phnum = 3U;

        Elf64_auxv_t auxv[Kernel::Process::STANDARD_AUXV_COUNT]{};
        const size_t auxv_count = Kernel::Process::populate_standard_auxv(
            loaded, auxv, Kernel::Process::STANDARD_AUXV_COUNT);
        REQUIRE(auxv_count == Kernel::Process::STANDARD_AUXV_COUNT);

        const char* const argv[] = {"/bin/sh", "-c", "echo milo"};
        const char* const envp[] = {"PATH=/bin:/usr/bin", "HOME=/root", nullptr};

        auto abi_res = Kernel::Process::UserStackBuilder::build_abi_layout(stack, 3, argv, envp,
                                                                           auxv, auxv_count);
        REQUIRE(abi_res.is_ok());

        const uintptr_t final_sp = abi_res.value();
        REQUIRE((final_sp % 16U) == 0U);
        CHECK(stack.final_sp_el0 == final_sp);
        CHECK(stack.argc == 3U);
        CHECK(stack.argv_vaddr == final_sp + 8U);

        // [SP + 0] == argc
        CHECK(read_stack_u64(stack, final_sp) == 3ULL);

        // [SP + 8 .. SP + 32] == argv[0..2], followed by NULL terminator
        const uintptr_t argv0_ptr = read_stack_u64(stack, final_sp + 8U);
        const uintptr_t argv1_ptr = read_stack_u64(stack, final_sp + 16U);
        const uintptr_t argv2_ptr = read_stack_u64(stack, final_sp + 24U);
        const uintptr_t argv_null = read_stack_u64(stack, final_sp + 32U);
        CHECK(argv_null == 0ULL);
        CHECK(std::strcmp(read_stack_cstr(stack, argv0_ptr), "/bin/sh") == 0);
        CHECK(std::strcmp(read_stack_cstr(stack, argv1_ptr), "-c") == 0);
        CHECK(std::strcmp(read_stack_cstr(stack, argv2_ptr), "echo milo") == 0);

        // envp[0..1], followed by NULL terminator
        const uintptr_t envp_base = final_sp + 40U;
        const uintptr_t envp0_ptr = read_stack_u64(stack, envp_base);
        const uintptr_t envp1_ptr = read_stack_u64(stack, envp_base + 8U);
        const uintptr_t envp_null = read_stack_u64(stack, envp_base + 16U);
        CHECK(envp_null == 0ULL);
        CHECK(std::strcmp(read_stack_cstr(stack, envp0_ptr), "PATH=/bin:/usr/bin") == 0);
        CHECK(std::strcmp(read_stack_cstr(stack, envp1_ptr), "HOME=/root") == 0);

        // Verify auxv entries including patched AT_RANDOM and AT_NULL terminator
        const uintptr_t auxv_base = envp_base + 24U;
        bool saw_at_random = false;
        bool saw_at_null = false;
        for (size_t idx = 0U; idx <= auxv_count; ++idx) {
            const uintptr_t entry_vaddr = auxv_base + (idx * sizeof(Elf64_auxv_t));
            const uint64_t a_type = read_stack_u64(stack, entry_vaddr);
            const uint64_t a_val = read_stack_u64(stack, entry_vaddr + 8U);

            if (a_type == AT_ENTRY) {
                CHECK(a_val == loaded.entry_point);
            } else if (a_type == AT_PAGESZ) {
                CHECK(a_val == Arch::PAGE_SIZE);
            } else if (a_type == AT_RANDOM) {
                saw_at_random = true;
                CHECK(a_val >= stack.virt_base);
                CHECK(a_val + 16U <= stack.virt_top);
            } else if (a_type == AT_NULL) {
                saw_at_null = true;
                CHECK(a_val == 0ULL);
            }
        }
        CHECK(saw_at_random);
        CHECK(saw_at_null);
    }

    SECTION("16-byte SP_EL0 alignment holds for both odd and even argc counts") {
        const char* const argv_even[] = {"prog", "arg1", "arg2", "arg3"};
        auto even_res = Kernel::Process::UserStackBuilder::build_abi_layout(stack, 4, argv_even);
        REQUIRE(even_res.is_ok());
        CHECK((even_res.value() % 16U) == 0U);
        CHECK(read_stack_u64(stack, even_res.value()) == 4ULL);

        const char* const argv_odd[] = {"prog", "arg1", "arg2"};
        auto odd_res = Kernel::Process::UserStackBuilder::build_abi_layout(stack, 3, argv_odd);
        REQUIRE(odd_res.is_ok());
        CHECK((odd_res.value() % 16U) == 0U);
        CHECK(read_stack_u64(stack, odd_res.value()) == 3ULL);
    }

    SECTION("Defensive error checks for null descriptors and stack overflow") {
        Kernel::Process::UserStack uninit_stack{};
        const char* const argv[] = {"prog"};
        auto null_stack_res =
            Kernel::Process::UserStackBuilder::build_abi_layout(uninit_stack, 1, argv);
        REQUIRE(null_stack_res.is_err());
        CHECK(null_stack_res.error() == Kernel::Process::StackBuilderError::NullArgument);

        auto null_argv_res = Kernel::Process::UserStackBuilder::build_abi_layout(stack, 1, nullptr);
        REQUIRE(null_argv_res.is_err());
        CHECK(null_argv_res.error() == Kernel::Process::StackBuilderError::NullArgument);

        // Small 1-page stack must reject an oversized argument payload with BufferOverflow
        auto tiny_stack_res = Kernel::Process::UserStackBuilder::create_and_map(
            address_space, 0x0000000020000000ULL, Arch::PAGE_SIZE);
        REQUIRE(tiny_stack_res.is_ok());
        Kernel::Process::UserStack tiny_stack = tiny_stack_res.value();

        char huge_arg[Arch::PAGE_SIZE]{};
        std::memset(huge_arg, 'A', sizeof(huge_arg) - 1U);
        huge_arg[sizeof(huge_arg) - 1U] = '\0';
        const char* const huge_argv[] = {huge_arg};

        auto overflow_res =
            Kernel::Process::UserStackBuilder::build_abi_layout(tiny_stack, 1, huge_argv);
        REQUIRE(overflow_res.is_err());
        CHECK(overflow_res.error() == Kernel::Process::StackBuilderError::BufferOverflow);

        free_frames(tiny_stack.phys_base, tiny_stack.num_pages);
    }

    free_frames(stack.phys_base, stack.num_pages);
    address_space.destroy();
}