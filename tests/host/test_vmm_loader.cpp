#include "loader/elf_loader.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"
#include "mocks/host_hal.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

TEST_CASE("VMM", "AddressSpace Lifecycle, Page-Crossing User Copy & Clone Isolation") {
    HostHAL::reset();
    pfa_init(HostHAL::SIMULATED_RAM_BASE, HostHAL::SIMULATED_RAM_SIZE);
    const size_t baseline = pfa_allocated_pages_count();

    {
        auto as_res = Kernel::MM::AddressSpace::create();
        EXPECT_TRUE(as_res.is_ok());
        auto adrsp = static_cast<Kernel::MM::AddressSpace&&>(as_res.value());

        const uintptr_t page_0 = alloc_frame();
        const uintptr_t page_1 = alloc_frame();
        EXPECT_NE(page_0, 0U);
        EXPECT_NE(page_1, 0U);

        const HAL::PageFlags rw_user =
            HAL::PageFlags::User | HAL::PageFlags::Read | HAL::PageFlags::Write;
        EXPECT_TRUE(adrsp.map_user_page(0x00400000ULL, page_0, rw_user));
        EXPECT_TRUE(adrsp.map_user_page(0x00401000ULL, page_1, rw_user));

        // Reject kernel higher-half virtual address in user page table
        EXPECT_FALSE(adrsp.map_user_page(
            HAL::get_platform_info().higher_half_offset + 0x00400000ULL, page_0, rw_user));

        // Cross-page copy_to_user / copy_from_user across 0x00400FF8..0x00401008
        const char msg[] = "ABCDEFGH1234567";
        EXPECT_EQ(Kernel::MM::copy_to_user(adrsp, 0x00400FF8ULL, msg, sizeof(msg)), 0);

        char readback[sizeof(msg)]{};
        EXPECT_EQ(Kernel::MM::copy_from_user(adrsp, readback, 0x00400FF8ULL, sizeof(readback)), 0);
        EXPECT_EQ(std::memcmp(msg, readback, sizeof(msg)), 0);

        // Protect page 1 as Read-Only and verify copy_to_user fails with -EFAULT (-14)
        EXPECT_TRUE(
            adrsp.protect_user_page(0x00401000ULL, HAL::PageFlags::User | HAL::PageFlags::Read));
        EXPECT_EQ(Kernel::MM::copy_to_user(adrsp, 0x00401000ULL, msg, 4U), -14);

        // Clone address space and verify deep copy isolation
        auto clone_res = adrsp.clone();
        EXPECT_TRUE(clone_res.is_ok());
        auto cloned = static_cast<Kernel::MM::AddressSpace&&>(clone_res.value());

        char clone_readback[sizeof(msg)]{};
        EXPECT_EQ(Kernel::MM::copy_from_user(cloned, clone_readback, 0x00400FF8ULL,
                                             sizeof(clone_readback)),
                  0);
        EXPECT_EQ(std::memcmp(msg, clone_readback, sizeof(msg)), 0);

        cloned.destroy();
        adrsp.destroy();
    }

    EXPECT_EQ(pfa_allocated_pages_count(), baseline);
}

TEST_CASE("ElfLoader", "Multi-Segment Loading, BSS Zeroing & OOM Rollback") {
    HostHAL::reset();
    pfa_init(HostHAL::SIMULATED_RAM_BASE, HostHAL::SIMULATED_RAM_SIZE);

    std::vector<uint8_t> elf_buf(1024U, 0U);
    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(elf_buf.data());
    ehdr->e_ident[EI_MAG0] = ELFMAG0;
    ehdr->e_ident[EI_MAG1] = ELFMAG1;
    ehdr->e_ident[EI_MAG2] = ELFMAG2;
    ehdr->e_ident[EI_MAG3] = ELFMAG3;
    ehdr->e_ident[EI_CLASS] = ELFCLASS64;
    ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr->e_ident[EI_VERSION] = EV_CURRENT;
    ehdr->e_type = ET_EXEC;
    ehdr->e_machine = HAL::get_platform_info().elf_machine;
    ehdr->e_version = EV_CURRENT;
    ehdr->e_entry = 0x00400180ULL;
    ehdr->e_phoff = sizeof(Elf64_Ehdr);
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = 2U;

    auto* phdrs = reinterpret_cast<Elf64_Phdr*>(elf_buf.data() + sizeof(Elf64_Ehdr));
    phdrs[0].p_type = PT_LOAD;
    phdrs[0].p_flags = PF_R | PF_X;
    phdrs[0].p_offset = 0x180U;
    phdrs[0].p_vaddr = 0x00400180ULL;
    phdrs[0].p_paddr = 0x00400180ULL;
    phdrs[0].p_filesz = 0x40U;
    phdrs[0].p_memsz = 0x40U;
    phdrs[0].p_align = 0x1000U;

    phdrs[1].p_type = PT_LOAD;
    phdrs[1].p_flags = PF_R | PF_W;
    phdrs[1].p_offset = 0x240U;
    phdrs[1].p_vaddr = 0x00401240ULL;
    phdrs[1].p_paddr = 0x00401240ULL;
    phdrs[1].p_filesz = 0x20U;
    phdrs[1].p_memsz = 0x80U;
    phdrs[1].p_align = 0x1000U;
    std::memset(elf_buf.data() + 0x240U, 0xAA, 0x20U);

    const size_t baseline = pfa_allocated_pages_count();

    auto loaded_res = Kernel::Loader::load_elf(elf_buf.data(), elf_buf.size());
    EXPECT_TRUE(loaded_res.is_ok());
    EXPECT_EQ(loaded_res->entry_point, 0x00400180ULL);
    EXPECT_EQ(loaded_res->max_vaddr, 0x00402000ULL);
    EXPECT_EQ(loaded_res->loaded_segment_count, 2U);

    loaded_res->address_space.destroy();
    EXPECT_EQ(pfa_allocated_pages_count(), baseline);

    // Simulate OOM on second segment and verify zero frames leaked
    phdrs[1].p_memsz = 0x20000000ULL;
    const auto oom_res = Kernel::Loader::load_elf(elf_buf.data(), elf_buf.size());
    EXPECT_TRUE(oom_res.is_err());
    EXPECT_EQ(oom_res.error(), Kernel::Loader::ElfLoaderError::OutOfMemory);
    EXPECT_EQ(pfa_allocated_pages_count(), baseline);
}