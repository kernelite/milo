#include "loader/elf64.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct TestElfImage {
    std::vector<uint8_t> buf;
    size_t valid_size{0U};
};

TestElfImage make_base_elf(Elf64_Half type = ET_EXEC) {
    TestElfImage img{};
    img.buf.resize(512U, 0U);

    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(img.buf.data());
    ehdr->e_ident[EI_MAG0] = ELFMAG0;
    ehdr->e_ident[EI_MAG1] = ELFMAG1;
    ehdr->e_ident[EI_MAG2] = ELFMAG2;
    ehdr->e_ident[EI_MAG3] = ELFMAG3;
    ehdr->e_ident[EI_CLASS] = ELFCLASS64;
    ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr->e_ident[EI_VERSION] = EV_CURRENT;
    ehdr->e_ident[EI_OSABI] = ELFOSABI_SYSV;
    ehdr->e_type = type;
    ehdr->e_machine = EM_AARCH64;
    ehdr->e_version = EV_CURRENT;
    ehdr->e_entry = 0x0000000040080000ULL;
    ehdr->e_phoff = sizeof(Elf64_Ehdr);
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = 3U;

    auto* phdrs = reinterpret_cast<Elf64_Phdr*>(img.buf.data() + sizeof(Elf64_Ehdr));

    // Segment 0: RX code segment
    phdrs[0].p_type = PT_LOAD;
    phdrs[0].p_flags = PF_R | PF_X;
    phdrs[0].p_offset = 0x100U;
    phdrs[0].p_vaddr = 0x00400100ULL;
    phdrs[0].p_paddr = 0x00400100ULL;
    phdrs[0].p_filesz = 0x40U;
    phdrs[0].p_memsz = 0x40U;
    phdrs[0].p_align = 0x1000U;

    // Segment 1: RW data + 0x50 bytes .bss
    phdrs[1].p_type = PT_LOAD;
    phdrs[1].p_flags = PF_R | PF_W;
    phdrs[1].p_offset = 0x140U;
    phdrs[1].p_vaddr = 0x00401140ULL;
    phdrs[1].p_paddr = 0x00401140ULL;
    phdrs[1].p_filesz = 0x20U;
    phdrs[1].p_memsz = 0x70U;
    phdrs[1].p_align = 0x1000U;

    // Segment 2: Non-loadable metadata segment
    phdrs[2].p_type = PT_GNU_STACK;
    phdrs[2].p_flags = PF_R | PF_W;
    phdrs[2].p_align = 16U;

    img.valid_size = 0x160U;
    return img;
}

Elf64_Ehdr* get_ehdr(TestElfImage& img) {
    return reinterpret_cast<Elf64_Ehdr*>(img.buf.data());
}

Elf64_Phdr* get_phdrs(TestElfImage& img) {
    return reinterpret_cast<Elf64_Phdr*>(img.buf.data() + sizeof(Elf64_Ehdr));
}

} // namespace

TEST_CASE("Elf64", "ValidExecAndDynBinaries") {
    EXPECT_EQ(sizeof(Elf64_Ehdr), 64U);
    EXPECT_EQ(sizeof(Elf64_Phdr), 56U);

    const auto exec_img = make_base_elf(ET_EXEC);
    const auto res_exec = Elf64View::parse(exec_img.buf.data(), exec_img.valid_size);
    EXPECT_TRUE(res_exec.is_ok());
    EXPECT_TRUE(res_exec->is_executable());
    EXPECT_FALSE(res_exec->is_position_independent());
    EXPECT_EQ(res_exec->entry_point(), 0x0000000040080000ULL);
    EXPECT_EQ(res_exec->segment_count(), 3U);
    EXPECT_EQ(res_exec->load_segment_count(), 2U);

    size_t load_count = 0U;
    uint64_t total_bss = 0ULL;
    res_exec->for_each_load_segment([&](const Elf64Segment& seg) {
        ++load_count;
        total_bss += seg.bss_size();
        if (seg.is_executable()) {
            EXPECT_TRUE(seg.is_readable());
            EXPECT_FALSE(seg.is_writable());
            EXPECT_EQ(seg.bss_size(), 0ULL);
        } else if (seg.is_writable()) {
            EXPECT_TRUE(seg.is_readable());
            EXPECT_EQ(seg.bss_size(), 0x50ULL);
        }
    });
    EXPECT_EQ(load_count, 2U);
    EXPECT_EQ(total_bss, 0x50ULL);

    const auto dyn_img = make_base_elf(ET_DYN);
    const auto res_dyn = Elf64View::parse(dyn_img.buf.data(), dyn_img.valid_size);
    EXPECT_TRUE(res_dyn.is_ok());
    EXPECT_TRUE(res_dyn->is_position_independent());
    EXPECT_FALSE(res_dyn->is_executable());
}

TEST_CASE("Elf64", "MalformationBoundsHandling") {
    const auto base_elf = make_base_elf(ET_EXEC);

    // BufferTooSmall
    EXPECT_EQ(Elf64View::parse(static_cast<const uint8_t*>(nullptr), 0U).error(),
              ElfStatus::BufferTooSmall);
    EXPECT_EQ(Elf64View::parse(static_cast<const void*>(nullptr), 128U).error(),
              ElfStatus::BufferTooSmall);
    EXPECT_EQ(Elf64View::parse(base_elf.buf.data(), 63U).error(), ElfStatus::BufferTooSmall);

    // InvalidMagic
    {
        auto bad = base_elf;
        get_ehdr(bad)->e_ident[EI_MAG1] = 'X';
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::InvalidMagic);
    }

    // UnsupportedClass
    {
        auto bad = base_elf;
        get_ehdr(bad)->e_ident[EI_CLASS] = ELFCLASS32;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::UnsupportedClass);
    }

    // UnsupportedEndianness
    {
        auto bad = base_elf;
        get_ehdr(bad)->e_ident[EI_DATA] = ELFDATA2MSB;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::UnsupportedEndianness);
    }

    // UnsupportedVersion
    {
        auto bad = base_elf;
        get_ehdr(bad)->e_ident[EI_VERSION] = EV_NONE;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::UnsupportedVersion);
    }

    // UnsupportedMachine
    {
        auto bad = base_elf;
        get_ehdr(bad)->e_machine = EM_X86_64;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::UnsupportedMachine);
    }

    // UnsupportedType
    {
        auto bad = base_elf;
        get_ehdr(bad)->e_type = ET_REL;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::UnsupportedType);
    }

    // MalformedProgramHeaderTable
    {
        auto bad = base_elf;
        get_ehdr(bad)->e_ehsize = 32U;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::MalformedProgramHeaderTable);
    }
    {
        auto bad = base_elf;
        get_ehdr(bad)->e_phnum = 0U;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::MalformedProgramHeaderTable);
    }
    {
        auto bad = base_elf;
        get_ehdr(bad)->e_phentsize = 32U;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::MalformedProgramHeaderTable);
    }
    {
        auto bad = base_elf;
        get_ehdr(bad)->e_phoff = 0xFFFFFFFFFFFFFF00ULL;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::MalformedProgramHeaderTable);
    }

    // SegmentOutOfBounds
    {
        auto bad = base_elf;
        get_phdrs(bad)[0].p_filesz = 0x80U;
        get_phdrs(bad)[0].p_memsz = 0x40U;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::SegmentOutOfBounds);
    }
    {
        auto bad = base_elf;
        get_phdrs(bad)[1].p_offset = bad.valid_size - 8U;
        get_phdrs(bad)[1].p_filesz = 0x20U;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::SegmentOutOfBounds);
    }

    // InvalidSegmentAlignment
    {
        auto bad = base_elf;
        get_phdrs(bad)[0].p_align = 6U;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::InvalidSegmentAlignment);
    }
    {
        auto bad = base_elf;
        get_phdrs(bad)[0].p_align = 0x1000U;
        get_phdrs(bad)[0].p_vaddr = 0x00400200ULL;
        EXPECT_EQ(Elf64View::parse(bad.buf.data(), bad.valid_size).error(),
                  ElfStatus::InvalidSegmentAlignment);
    }
}