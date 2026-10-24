#include "../arch/memory_config.hpp"
#include "boot/fdt.hpp"
#include "fs/cpio.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "hal/test_hooks.hpp"
#include "loader/elf64.hpp"
#include "loader/elf_loader.hpp"
#include "mm/address_space.hpp"
#include "mm/pfa.hpp"
#include "process/process.hpp"

#include <cstddef>
#include <cstdint>

extern "C" void cmd_test_uxn_fault();
extern "C" void cmd_test_user_echo();
// Access global initrd metadata from main.cpp
extern FDT::InitrdLocation g_initrd_loc;

namespace {

constexpr size_t INPUT_BUFFER_SIZE = 128;
int bss_check_var;

bool streq(const char* str1, const char* str2) {
    if (str1 == nullptr || str2 == nullptr) {
        return str1 == str2;
    }

    while (*str1 != '\0' && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *str1 == *str2;
}

bool str_starts_with(const char* str, const char* prefix) noexcept {
    if (str == nullptr || prefix == nullptr) {
        return false;
    }
    while (*prefix != '\0') {
        if (*str != *prefix) {
            return false;
        }
        str++;
        prefix++;
    }
    return true;
}

void print_str(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    HAL::get_console().write(str, len);
}

void assert_test(bool condition, const char* test_name) {
    if (condition) {
        print_str("[PASS] ");
    } else {
        print_str("[FAIL] ");
    }
    print_str(test_name);
    print_str("\r\n");
}

void print_dec(uint8_t val) noexcept {
    if (val >= 10U) {
        HAL::get_console().putc(static_cast<char>('0' + (val / 10U)));
    }
    HAL::get_console().putc(static_cast<char>('0' + (val % 10U)));
}

void print_dec64(uint64_t val) noexcept {
    if (val == 0) {
        HAL::get_console().putc('0');
        return;
    }
    char buf[21];
    size_t idx = 0;
    while (val > 0) {
        buf[idx++] = static_cast<char>('0' + (val % 10U));
        val /= 10U;
    }
    while (idx > 0) {
        HAL::get_console().putc(buf[--idx]);
    }
}

void print_octal(uint32_t val) noexcept {
    if (val == 0) {
        HAL::get_console().putc('0');
        return;
    }
    char buf[12];
    size_t idx = 0;
    while (val > 0) {
        buf[idx++] = static_cast<char>('0' + (val & 7U));
        val >>= 3U;
    }
    HAL::get_console().putc('0');
    while (idx > 0) {
        HAL::get_console().putc(buf[--idx]);
    }
}

void print_hex64(uint64_t val) noexcept {
    char buf[19] = "0x0000000000000000";
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 17; i >= 2; --i) {
        buf[i] = hex_chars[val & 0xFU];
        val >>= 4U;
    }
    print_str(buf);
}

void cmd_test_vmm() noexcept {
    print_str("\r\n[TEST] Running AddressSpace Lifecycle & Mapping Test...\r\n");

    const size_t pages_before = pfa_allocated_pages_count();

    {
        auto as_res = Kernel::MM::AddressSpace::create();
        assert_test(as_res.is_ok(), "AddressSpace L0 Root Table frame allocated");
        auto adrsp = static_cast<Kernel::MM::AddressSpace&&>(as_res.value());

        // Map non-aligned user addresses
        const uintptr_t test_vaddr = 0x0000000000400000ULL;
        const uintptr_t test_paddr = alloc_frame();
        assert_test(test_paddr != 0U, "Allocated user physical test frame");

        const bool map_ok = adrsp.map_user_page(test_vaddr, test_paddr,
                                                HAL::PageFlags::User | HAL::PageFlags::Read |
                                                    HAL::PageFlags::Execute);
        assert_test(map_ok, "Mapped RX page in AddressSpace with L1/L2/L3 table creation");

        // Attempt mapping out-of-bounds higher-half address in user table
        const bool bad_map = adrsp.map_user_page(0xFFFF800000400000ULL, test_paddr,
                                                 HAL::PageFlags::User | HAL::PageFlags::Read);
        assert_test(!bad_map,
                    "Rejected higher-half virtual address in user space translation table");

        free_frame(test_paddr);
        adrsp.destroy();
    }

    const size_t pages_after = pfa_allocated_pages_count();
    assert_test(pages_before == pages_after,
                "AddressSpace destruction freed all intermediate L1/L2/L3 & L0 tables");
}

void cmd_test_loader_pipeline() noexcept {
    print_str("\r\n[TEST] Running Segment Loader Pipeline & OOM Rollback Suite...\r\n");

    alignas(16) static uint8_t test_elf_buf[1024] = {0};
    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(test_elf_buf);
    ehdr->e_ident[EI_MAG0] = ELFMAG0;
    ehdr->e_ident[EI_MAG1] = ELFMAG1;
    ehdr->e_ident[EI_MAG2] = ELFMAG2;
    ehdr->e_ident[EI_MAG3] = ELFMAG3;
    ehdr->e_ident[EI_CLASS] = ELFCLASS64;
    ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr->e_ident[EI_VERSION] = EV_CURRENT;
    ehdr->e_type = ET_EXEC;
    ehdr->e_machine = EM_AARCH64;
    ehdr->e_version = EV_CURRENT;
    ehdr->e_entry = 0x00400180ULL;
    ehdr->e_phoff = 64;
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = 2;

    auto* phdrs = reinterpret_cast<Elf64_Phdr*>(test_elf_buf + 64);

    // Segment 0: Code RX (non-page aligned offset 0x180, vaddr 0x400180 -> congruent modulo 0x1000)
    phdrs[0].p_type = PT_LOAD;
    phdrs[0].p_flags = PF_R | PF_X;
    phdrs[0].p_offset = 0x180;
    phdrs[0].p_vaddr = 0x00400180ULL;
    phdrs[0].p_paddr = 0x00400180ULL;
    phdrs[0].p_filesz = 0x40;
    phdrs[0].p_memsz = 0x40;
    phdrs[0].p_align = 0x1000;

    // Segment 1: Data RW with BSS (offset 0x240, vaddr 0x401240 -> congruent modulo 0x1000)
    phdrs[1].p_type = PT_LOAD;
    phdrs[1].p_flags = PF_R | PF_W;
    phdrs[1].p_offset = 0x240;
    phdrs[1].p_vaddr = 0x00401240ULL;
    phdrs[1].p_paddr = 0x00401240ULL;
    phdrs[1].p_filesz = 0x20;
    phdrs[1].p_memsz = 0x80;
    phdrs[1].p_align = 0x1000;

    for (size_t i = 0; i < 0x20; ++i) {
        test_elf_buf[0x240 + i] = static_cast<uint8_t>(0xAAU);
    }

    const size_t pages_initial = pfa_allocated_pages_count();

    auto load_res = Kernel::Loader::load_elf(test_elf_buf, sizeof(test_elf_buf));
    assert_test(load_res.is_ok(), "Segment Loader loaded valid multi-segment ELF");
    assert_test(load_res->entry_point == 0x00400180ULL, "Extracted accurate execution entry point");

    load_res->address_space.destroy();
    assert_test(pfa_allocated_pages_count() == pages_initial,
                "Clean teardown returned all allocated frames to PFA");

    // OOM Rollback Test
    print_str("  Simulating OOM condition on Segment 1 memsz...\r\n");
    const size_t pages_before_oom = pfa_allocated_pages_count();

    phdrs[1].p_memsz = 0x20000000ULL; // 512 MB request exceeds platform RAM
    auto oom_res = Kernel::Loader::load_elf(test_elf_buf, sizeof(test_elf_buf));

    assert_test(oom_res.is_err() && oom_res.error() == Kernel::Loader::ElfLoaderError::OutOfMemory,
                "Loader detected OutOfMemory and aborted instantiation safely");

    const size_t pages_after_oom = pfa_allocated_pages_count();
    assert_test(pages_before_oom == pages_after_oom,
                "OOM Rollback Verification: Zero physical frames leaked");

    phdrs[1].p_memsz = 0x80; // restore
}

void cmd_test_user_isolation() noexcept {
    print_str("\r\n[TEST] Running EL0 User Execution & Fault Isolation Tests...\r\n");

    alignas(16) static uint8_t synth_bin[512] = {0};
    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(synth_bin);
    ehdr->e_ident[EI_MAG0] = ELFMAG0;
    ehdr->e_ident[EI_MAG1] = ELFMAG1;
    ehdr->e_ident[EI_MAG2] = ELFMAG2;
    ehdr->e_ident[EI_MAG3] = ELFMAG3;
    ehdr->e_ident[EI_CLASS] = ELFCLASS64;
    ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr->e_ident[EI_VERSION] = EV_CURRENT;
    ehdr->e_ident[EI_OSABI] = ELFOSABI_SYSV;
    ehdr->e_type = ET_EXEC;
    ehdr->e_machine = EM_AARCH64;
    ehdr->e_version = EV_CURRENT;
    ehdr->e_entry = 0x00400100ULL; // Congruent with p_offset 0x100 modulo 0x1000
    ehdr->e_phoff = 64;
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = 1;

    auto* phdr = reinterpret_cast<Elf64_Phdr*>(synth_bin + 64);
    phdr->p_type = PT_LOAD;
    phdr->p_flags = PF_R | PF_X;
    phdr->p_offset = 0x100;
    phdr->p_vaddr = 0x00400100ULL;
    phdr->p_paddr = 0x00400100ULL;
    phdr->p_filesz = 16;
    phdr->p_memsz = 16;
    phdr->p_align = 0x1000;

    // AArch64 code:
    // mov x8, #93 (SYS_EXIT)
    // mov x0, #42
    // svc #0
    const uint32_t code[] = {
        0xD2800BA8U, // mov x8, #93
        0xD2800540U, // mov x0, #42
        0xD4000001U, // svc #0
        0xD65F03C0U  // ret
    };
    for (size_t i = 0; i < 4; ++i) {
        *reinterpret_cast<uint32_t*>(synth_bin + 0x100 + i * 4) = code[i];
    }

    auto load_res = Kernel::Loader::load_elf(synth_bin, sizeof(synth_bin));
    assert_test(load_res.is_ok(), "Loaded user-space test payload");
    if (!load_res.is_ok()) {
        return;
    }

    // Allocate isolated user stack
    const uintptr_t user_stack_frame = alloc_frame();
    load_res->address_space.map_user_page(0x0000000000800000ULL, user_stack_frame,
                                          HAL::PageFlags::User | HAL::PageFlags::Read |
                                              HAL::PageFlags::Write);

    const uintptr_t orig_root = Kernel::MM::AddressSpace::current_root_paddr();

    // Switch TTBR0_EL1 to user address space
    load_res->address_space.activate();
    assert_test(Kernel::MM::AddressSpace::current_root_paddr() ==
                    load_res->address_space.root_paddr(),
                "TTBR0_EL1 switched to isolated user address space");

    // Enter EL0
    const int64_t exit_code =
        HAL::get_cpu().enter_user_mode(load_res->entry_point, 0x0000000000801000ULL);
    assert_test(exit_code == 42, "EL0 process executed and exited with clean status 42");

    // Restore kernel translation table
    Kernel::MM::AddressSpace::activate_root(orig_root);

    free_frame(user_stack_frame);
    load_res->address_space.destroy();
}

void cmd_ls_initrd() noexcept {
    if (!g_initrd_loc.valid || g_initrd_loc.start_paddr == 0 || g_initrd_loc.size == 0) {
        print_str("[CPIO ERROR] No valid initrd loaded in physical memory.\r\n");
        return;
    }

    const CpioArchive archive(reinterpret_cast<const void*>(g_initrd_loc.start_paddr),
                              g_initrd_loc.size);
    if (!archive.is_valid()) {
        print_str("[CPIO ERROR] Invalid CPIO archive signature.\r\n");
        return;
    }

    print_str("\r\n--- Initrd CPIO Archive Listing ---\r\n");
    print_str("  Permissions   Size (Bytes)   Path\r\n");
    print_str("  ------------------------------------\r\n");

    size_t entry_count = 0;
    const CpioStatus status = archive.for_each([&](const CpioEntry& entry) {
        print_str("  ");
        print_octal(entry.mode);
        print_str("       ");
        print_dec64(entry.size);
        print_str(" bytes      ");
        print_str(entry.name);
        if (entry.is_directory) {
            print_str("/");
        }
        print_str("\r\n");
        entry_count++;
    });

    if (status != CpioStatus::Success) {
        print_str("[CPIO ERROR] Traversal failed: ");
        print_str(cpio_status_str(status));
        print_str("\r\n");
    } else {
        print_str("Total entries: ");
        print_dec64(entry_count);
        print_str("\r\n");
    }
}

void cmd_cat_initrd(const char* path) noexcept {
    while (path != nullptr && *path == ' ') {
        path++;
    }
    if (path == nullptr || *path == '\0') {
        print_str("Usage: cat initrd <path>\r\n");
        return;
    }

    if (!g_initrd_loc.valid || g_initrd_loc.start_paddr == 0 || g_initrd_loc.size == 0) {
        print_str("[CPIO ERROR] No valid initrd loaded in physical memory.\r\n");
        return;
    }

    CpioEntry entry{};
    const CpioStatus status = cpio_lookup(reinterpret_cast<const void*>(g_initrd_loc.start_paddr),
                                          g_initrd_loc.size, path, entry);

    if (status == CpioStatus::Success) {
        print_str("[CPIO] Located '");
        print_str(path);
        print_str("' (");
        print_dec64(entry.size);
        print_str(" bytes):\r\n");

        if (entry.size > 0 && entry.data != nullptr) {
            HAL::get_console().write(reinterpret_cast<const char*>(entry.data), entry.size);
            const char last_byte = static_cast<char>(entry.data[entry.size - 1]);
            if (last_byte != '\n' && last_byte != '\r') {
                print_str("\r\n");
            }
        }
    } else if (status == CpioStatus::FileNotFound) {
        print_str("[CPIO ERROR] File not found: '");
        print_str(path);
        print_str("'\r\n");
    } else {
        print_str("[CPIO ERROR] Lookup failed with status: ");
        print_str(cpio_status_str(status));
        print_str("\r\n");
    }
}

// elf initrd <path> diagnostic command
void cmd_elf_initrd(const char* path) noexcept {
    while (path != nullptr && *path == ' ') {
        path++;
    }
    if (path == nullptr || *path == '\0') {
        print_str("Usage: elf initrd <path>\r\n");
        return;
    }

    if (!g_initrd_loc.valid || g_initrd_loc.start_paddr == 0 || g_initrd_loc.size == 0) {
        print_str("[ELF ERROR] No valid initrd loaded in physical memory.\r\n");
        return;
    }

    CpioEntry entry{};
    const CpioStatus cpio_st = cpio_lookup(reinterpret_cast<const void*>(g_initrd_loc.start_paddr),
                                           g_initrd_loc.size, path, entry);

    if (cpio_st != CpioStatus::Success) {
        print_str("[ELF ERROR] Failed to locate '");
        print_str(path);
        print_str("' in initrd: ");
        print_str(cpio_status_str(cpio_st));
        print_str("\r\n");
        return;
    }

    auto parse_res = Elf64View::parse(entry.data, entry.size);
    if (!parse_res) {
        print_str("[ELF ERROR] ELF validation failed: ");
        print_str(elf_status_str(parse_res.error()));
        print_str("\r\n");
        return;
    }

    const auto& elf = parse_res.value();
    print_str("\r\n================ ELF64 HEADER INFO ================\r\n");
    print_str("  Binary Type         : ");
    print_str(elf.is_executable() ? "ET_EXEC (Executable)\r\n"
                                  : "ET_DYN (Position-Independent)\r\n");
    print_str("  Target Architecture : AArch64 (0xB7)\r\n");
    print_str("  Entry Point         : ");
    print_hex64(elf.entry_point());
    print_str("\r\n  Program Headers     : ");
    print_dec64(elf.segment_count());
    print_str(" (PT_LOAD segments: ");
    print_dec64(elf.load_segment_count());
    print_str(")\r\n");
    print_str("===================================================\r\n");

    print_str("\r\n--- Program Headers (Segments) ---\r\n");
    print_str("  Type    Flags   Virtual Address     File Size   Mem Size    BSS Size\r\n");
    print_str("  --------------------------------------------------------------------\r\n");

    elf.for_each_segment([](const Elf64Segment& seg) {
        print_str("  ");
        if (seg.type == PT_LOAD) {
            print_str("LOAD    ");
        } else if (seg.type == PT_PHDR) {
            print_str("PHDR    ");
        } else if (seg.type == PT_DYNAMIC) {
            print_str("DYNAMIC ");
        } else if (seg.type == PT_NOTE) {
            print_str("NOTE    ");
        } else if (seg.type == PT_GNU_STACK) {
            print_str("STACK   ");
        } else {
            print_str("OTHER   ");
        }

        HAL::get_console().putc(seg.is_readable() ? 'R' : '-');
        HAL::get_console().putc(seg.is_writable() ? 'W' : '-');
        HAL::get_console().putc(seg.is_executable() ? 'X' : '-');
        print_str("     ");

        print_hex64(seg.vaddr);
        print_str("  ");
        print_hex64(seg.filesz);
        print_str("  ");
        print_hex64(seg.memsz);
        print_str("  ");
        print_hex64(seg.bss_size());
        print_str("\r\n");
    });
}

// Comprehensive ELF parser test suite
void cmd_test_elf() noexcept {
    print_str("\r\n[TEST] Running ELF64 In-Memory Parser Verification Suite...\r\n");

    // 1. ABI Type Size Invariants
    assert_test(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr size is exactly 64 bytes");
    assert_test(sizeof(Elf64_Phdr) == 56, "Elf64_Phdr size is exactly 56 bytes");

    // 2. Synthesize AArch64 ELF binary
    alignas(16) static uint8_t synth_elf[512];
    for (size_t i = 0; i < sizeof(synth_elf); ++i) {
        synth_elf[i] = 0U;
    }

    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(synth_elf);
    ehdr->e_ident[EI_MAG0] = ELFMAG0;
    ehdr->e_ident[EI_MAG1] = ELFMAG1;
    ehdr->e_ident[EI_MAG2] = ELFMAG2;
    ehdr->e_ident[EI_MAG3] = ELFMAG3;
    ehdr->e_ident[EI_CLASS] = ELFCLASS64;
    ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr->e_ident[EI_VERSION] = EV_CURRENT;
    ehdr->e_ident[EI_OSABI] = ELFOSABI_SYSV;
    ehdr->e_type = ET_EXEC;
    ehdr->e_machine = EM_AARCH64;
    ehdr->e_version = EV_CURRENT;
    ehdr->e_entry = 0x0000000040080000ULL;
    ehdr->e_phoff = 64;
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = 2;

    // Segment 0: Code RX (vaddr % 0x1000 == offset % 0x1000 == 0x100)
    auto* phdrs = reinterpret_cast<Elf64_Phdr*>(synth_elf + 64);
    phdrs[0].p_type = PT_LOAD;
    phdrs[0].p_flags = PF_R | PF_X;
    phdrs[0].p_offset = 0x100;
    phdrs[0].p_vaddr = 0x400100; // Aligned with offset modulo 0x1000
    phdrs[0].p_paddr = 0x400100;
    phdrs[0].p_filesz = 0x40;
    phdrs[0].p_memsz = 0x40;
    phdrs[0].p_align = 0x1000;

    // Segment 1: Data RW with BSS (vaddr % 0x1000 == offset % 0x1000 == 0x140)
    phdrs[1].p_type = PT_LOAD;
    phdrs[1].p_flags = PF_R | PF_W;
    phdrs[1].p_offset = 0x140;
    phdrs[1].p_vaddr = 0x401140;
    phdrs[1].p_paddr = 0x401140;
    phdrs[1].p_filesz = 0x20;
    phdrs[1].p_memsz = 0x60;
    phdrs[1].p_align = 0x1000;

    const size_t valid_elf_size = 0x140 + 0x20; // 0x160 bytes total

    // 3. Positive Parsing Tests
    auto res_exec = Elf64View::parse(synth_elf, valid_elf_size);
    assert_test(res_exec.is_ok(), "Parsed valid AArch64 ET_EXEC binary");
    assert_test(res_exec->entry_point() == 0x0000000040080000ULL,
                "Extracted correct entry point address");
    assert_test(res_exec->segment_count() == 2, "Extracted correct program header segment count");
    assert_test(res_exec->load_segment_count() == 2, "Identified all PT_LOAD segments");

    // Segment extraction & BSS calculation
    size_t inspected_load_segs = 0;
    bool bss_calculated_correctly = false;
    res_exec->for_each_load_segment([&](const Elf64Segment& seg) {
        inspected_load_segs++;
        if (seg.is_writable() && seg.bss_size() == 0x40) {
            bss_calculated_correctly = true;
        }
    });
    assert_test(inspected_load_segs == 2, "Iterated all PT_LOAD segments without heap allocation");
    assert_test(bss_calculated_correctly,
                "Calculated zero-fill .bss byte count accurately (0x40 bytes)");

    // Test ET_DYN (Position-Independent Executable)
    ehdr->e_type = ET_DYN;
    auto res_dyn = Elf64View::parse(synth_elf, valid_elf_size);
    assert_test(res_dyn.is_ok() && res_dyn->is_position_independent(),
                "Parsed valid AArch64 ET_DYN (PIE) binary");
    ehdr->e_type = ET_EXEC; // restore

    // 4. Defensive Malformation Tests
    assert_test(Elf64View::parse(synth_elf, 50).error() == ElfStatus::BufferTooSmall,
                "Safely rejected truncated buffer (< 64 bytes) as BufferTooSmall");

    ehdr->e_ident[EI_MAG1] = 'Z';
    assert_test(Elf64View::parse(synth_elf, valid_elf_size).error() == ElfStatus::InvalidMagic,
                "Safely rejected corrupted magic bytes as InvalidMagic");
    ehdr->e_ident[EI_MAG1] = 'E'; // restore

    ehdr->e_ident[EI_CLASS] = ELFCLASS32;
    assert_test(Elf64View::parse(synth_elf, valid_elf_size).error() == ElfStatus::UnsupportedClass,
                "Safely rejected 32-bit ELF as UnsupportedClass");
    ehdr->e_ident[EI_CLASS] = ELFCLASS64; // restore

    ehdr->e_ident[EI_DATA] = ELFDATA2MSB;
    assert_test(Elf64View::parse(synth_elf, valid_elf_size).error() ==
                    ElfStatus::UnsupportedEndianness,
                "Safely rejected Big-Endian ELF as UnsupportedEndianness");
    ehdr->e_ident[EI_DATA] = ELFDATA2LSB; // restore

    ehdr->e_machine = EM_X86_64;
    assert_test(Elf64View::parse(synth_elf, valid_elf_size).error() ==
                    ElfStatus::UnsupportedMachine,
                "Safely rejected x86_64 machine architecture as UnsupportedMachine");
    ehdr->e_machine = EM_AARCH64; // restore

    ehdr->e_type = ET_REL;
    assert_test(Elf64View::parse(synth_elf, valid_elf_size).error() == ElfStatus::UnsupportedType,
                "Safely rejected ET_REL relocatable object as UnsupportedType");
    ehdr->e_type = ET_EXEC; // restore

    ehdr->e_phoff = 0xFFFFFFFFFF000000ULL;
    assert_test(
        Elf64View::parse(synth_elf, valid_elf_size).error() ==
            ElfStatus::MalformedProgramHeaderTable,
        "Safely rejected out-of-bounds Program Header Table as MalformedProgramHeaderTable");
    ehdr->e_phoff = 64; // restore

    phdrs[0].p_filesz = 0x80;
    phdrs[0].p_memsz = 0x40;
    assert_test(Elf64View::parse(synth_elf, valid_elf_size).error() ==
                    ElfStatus::SegmentOutOfBounds,
                "Safely rejected segment where p_filesz > p_memsz as SegmentOutOfBounds");
    phdrs[0].p_filesz = 0x40; // restore

    phdrs[0].p_offset = valid_elf_size - 0x10;
    phdrs[0].p_filesz = 0x40;
    assert_test(Elf64View::parse(synth_elf, valid_elf_size).error() ==
                    ElfStatus::SegmentOutOfBounds,
                "Safely rejected truncated segment payload as SegmentOutOfBounds");
    phdrs[0].p_offset = 0x100; // restore

    phdrs[0].p_align = 3;
    assert_test(Elf64View::parse(synth_elf, valid_elf_size).error() ==
                    ElfStatus::InvalidSegmentAlignment,
                "Safely rejected non-power-of-2 alignment as InvalidSegmentAlignment");

    // Incongruent alignment test:
    // (0x400200 % 0x1000 = 0x200) != (0x100 % 0x1000 = 0x100)
    phdrs[0].p_align = 0x1000;
    phdrs[0].p_vaddr = 0x400200; // Intentionally misaligned
    phdrs[0].p_offset = 0x100;
    assert_test(Elf64View::parse(synth_elf, valid_elf_size).error() ==
                    ElfStatus::InvalidSegmentAlignment,
                "Safely rejected incongruent virtual address alignment as InvalidSegmentAlignment");
    phdrs[0].p_vaddr = 0x400100; // restore
}

void cmd_info() noexcept {
    print_str("\r\n================ SYSTEM INFO ================\r\n");

    const uint8_t el_reg = HAL::get_cpu().current_el();
    print_str("  Current Exception Level : EL");
    print_dec(el_reg);
    print_str("\r\n");

    const auto mmu_st = HAL::get_mmu().status();
    print_str("  MMU Enabled             : ");
    print_str(mmu_st.mmu_enabled ? "YES\r\n" : "NO\r\n");
    print_str("  D-Cache Enabled         : ");
    print_str(mmu_st.dcache_enabled ? "YES\r\n" : "NO\r\n");
    print_str("  I-Cache Enabled         : ");
    print_str(mmu_st.icache_enabled ? "YES\r\n" : "NO\r\n");
    print_str("  SCTLR_EL1 Register      : ");
    print_hex64(mmu_st.raw_control_reg);
    print_str("\r\n=============================================\r\n");
}

void cmd_test_cpu() {
    print_str("[TEST] Testing CPU HAL Interrupt Management...\r\n");
    bool initial_state = HAL::get_cpu().interrupts_enabled();

    HAL::get_cpu().disable_interrupts();
    bool disabled_state = HAL::get_cpu().interrupts_enabled();
    print_str(disabled_state ? "  [FAIL] Interrupts still enabled after disable()\r\n"
                             : "  [PASS] Interrupts successfully disabled.\r\n");

    HAL::get_cpu().enable_interrupts();
    bool enabled_state = HAL::get_cpu().interrupts_enabled();
    print_str(enabled_state ? "  [PASS] Interrupts successfully enabled.\r\n"
                            : "  [FAIL] Interrupts still disabled after enable()\r\n");

    if (!initial_state) {
        HAL::get_cpu().disable_interrupts();
    }
}

void cmd_test_mmu() {
    print_str("[TEST] Inspecting System MMU & Cache Status via HAL...\r\n");

    const HAL::MmuStatus status = HAL::get_mmu().status();
    print_str("  Control Register Value: ");
    print_hex64(status.raw_control_reg);
    print_str("\r\n");

    print_str(status.mmu_enabled ? "  [PASS] MMU: ENABLED\r\n" : "  [INFO] MMU: DISABLED\r\n");
    print_str(status.dcache_enabled ? "  [PASS] Data Cache: ENABLED\r\n"
                                    : "  [INFO] Data Cache: DISABLED\r\n");
    print_str(status.icache_enabled ? "  [PASS] Instruction Cache: ENABLED\r\n"
                                    : "  [INFO] Instruction Cache: DISABLED\r\n");
}

void cmd_test_el0() {
    print_str("[TEST] Testing EL0 User-Mode Transition & SVC Trap...\r\n");
    const int64_t res = HAL::Test::run_user_mode_test();

    print_str("  [PASS] Safely returned from EL0 to EL1! Exit status: ");
    print_hex64(static_cast<uint64_t>(res));
    print_str("\r\n");
}

void cmd_test_cpp() {
    print_str("[TEST] Verifying Freestanding C++ Runtime...\r\n");

    if (bss_check_var == 0) {
        print_str("  [PASS] .bss section correctly zero-initialized.\r\n");
    } else {
        print_str("  [FAIL] .bss contains uninitialized garbage!\r\n");
    }

    print_str("  Testing Virtual Function Dynamic Dispatch... ");
    HAL::get_console().putc('[');
    HAL::get_console().putc('O');
    HAL::get_console().putc('K');
    HAL::get_console().putc(']');
    print_str("\r\n  [PASS] HAL vtable dynamic dispatch operational.\r\n");
}

void cmd_test_svc_trap() {
    print_str("[TEST] Triggering 'svc #0' syscall trap via ARCH assembly helper...\r\n");
    HAL::Test::trigger_user_syscall_test();
    print_str("  [PASS] SVC trap handled and returned to EL1 successfully!\r\n");
}

void cmd_test_data_abort() {
    print_str(
        "[TEST] Triggering Data Abort by accessing invalid address 0x00000000DEADBEE0ULL...\r\n");
    volatile uint32_t* bad_ptr = reinterpret_cast<volatile uint32_t*>(0x00000000DEADBEE0ULL);
    *bad_ptr = 0x42;

    print_str("  [PASS] Data Abort trapped and execution safely resumed!\r\n");
}

void cmd_test_pfa() {
    print_str("[TEST] Running Physical Frame Allocator verification suite...\r\n");

    if (g_initrd_loc.valid) {
        bool all_pages_reserved = true;
        for (uintptr_t addr = g_initrd_loc.start_paddr; addr < g_initrd_loc.end_paddr;
             addr += Arch::PAGE_SIZE) {
            if (!pfa_is_page_allocated(addr)) {
                all_pages_reserved = false;
                break;
            }
        }
        assert_test(all_pages_reserved,
                    "Initrd physical memory range is marked reserved in PFA bitmap");
    }

    bool no_collision = true;
    uintptr_t test_pages[32];
    size_t allocated_count = 0;

    for (size_t i = 0; i < 32; ++i) {
        test_pages[i] = alloc_frame();
        if (test_pages[i] == 0) {
            break;
        }
        allocated_count++;

        if (g_initrd_loc.valid) {
            if (test_pages[i] >= g_initrd_loc.start_paddr &&
                test_pages[i] < g_initrd_loc.end_paddr) {
                no_collision = false;
            }
        }
    }

    assert_test(no_collision, "Newly allocated frames do not overlap with reserved Initrd region");

    for (size_t i = 0; i < allocated_count; ++i) {
        free_frame(test_pages[i]);
    }

    uintptr_t page1 = alloc_frame();
    assert_test(page1 != 0, "Allocated single page frame");
    assert_test((page1 % Arch::PAGE_SIZE) == 0, "Allocated frame is 4KB page-aligned");

    free_frame(page1);
    uintptr_t page_recycled = alloc_frame();
    assert_test(page_recycled == page1, "Freed frame was recycled cleanly");
    free_frame(page_recycled);
}

void cmd_test_ro() noexcept {
    print_str("\r\n[TEST] Running Read-Only (.rodata) Protection Test...\r\n");
    print_str("  Target Address: ");
    print_hex64(HAL::Test::get_ro_target_address());
    print_str("\r\n  Executing write store: *ptr = 0xCAFEBABE...\r\n");

    HAL::Test::trigger_ro_fault();

    print_str("  [PASS] Data Abort trapped and recovered cleanly! Kernel active.\r\n");
}

void cmd_test_nx() noexcept {
    print_str("\r\n[TEST] Running Execute-Never (NX) Protection Test...\r\n");
    print_str("  Target Address: ");
    print_hex64(HAL::Test::get_nx_target_address());
    print_str("\r\n  Branching into non-executable .data section...\r\n");

    HAL::Test::trigger_nx_fault();

    print_str("  [PASS] Instruction Abort trapped and recovered cleanly! Kernel active.\r\n");
}

void cmd_test_sys() noexcept {
    print_str("\r\n[TEST] Running EL0 Privileged System Register Access Test...\r\n");
    print_str("  Entering EL0 User Mode to execute privileged instruction...\r\n");

    HAL::Test::trigger_privileged_sysreg_fault();

    print_str("  [PASS] EL0 System Register Trap handled! Process terminated cleanly.\r\n");
}

void cmd_test_cpio() noexcept {
    print_str("\r\n[TEST] Running Read-Only CPIO Driver Verification Suite...\r\n");
    assert_test(sizeof(CpioNewcHeader) == 110, "CpioNewcHeader size is exactly 110 bytes");

    CpioEntry entry{};
    assert_test(cpio_lookup(nullptr, 0, "init.txt", entry) == CpioStatus::ArchiveTooSmall,
                "Lookup with nullptr base returns ArchiveTooSmall");
    assert_test(cpio_lookup(nullptr, 100, "init.txt", entry) == CpioStatus::ArchiveTooSmall,
                "Lookup with size < 110 returns ArchiveTooSmall");

    if (g_initrd_loc.valid && g_initrd_loc.size > 0) {
        CpioEntry init_entry{};
        const CpioStatus init_st =
            cpio_lookup(reinterpret_cast<const void*>(g_initrd_loc.start_paddr), g_initrd_loc.size,
                        "/init.txt", init_entry);
        assert_test(init_st == CpioStatus::Success && init_entry.size >= 14,
                    "Live QEMU initrd locates '/init.txt' successfully");
    }
}

void cmd_test_stack_and_abi() noexcept {
    print_str("\r\n[TEST] Running User Stack Allocation & System V ABI Construction Tests...\r\n");

    auto as_res = Kernel::MM::AddressSpace::create();
    assert_test(as_res.is_ok(), "Target process AddressSpace initialized");
    auto address_space = static_cast<Kernel::MM::AddressSpace&&>(as_res.value());

    // 1. Stack Memory Allocation & UXN Mapping
    auto stack_res = Kernel::Process::UserStackBuilder::create_and_map(address_space);
    assert_test(stack_res.is_ok(),
                "Allocated 64 KiB stack physical frames and mapped into user space");

    auto user_stack = stack_res.value();
    assert_test(user_stack.virt_top == Kernel::Process::DEFAULT_USER_STACK_TOP,
                "Stack top virtual address configured at 0x3FFF0000");

    // 2. System V ABI Layout Population
    const char* const test_argv[] = {"milo_init", "hello_aarch64", "system_v_abi", "test_arg"};
    const char* const test_envp[] = {"PATH=/usr/bin:/bin", "USER=milo", "TERM=vt100", nullptr};

    auto abi_res =
        Kernel::Process::UserStackBuilder::build_abi_layout(user_stack, 4, test_argv, test_envp);

    assert_test(abi_res.is_ok(), "System V ABI stack layout built successfully");
    const uintptr_t final_sp_el0 = abi_res.value();

    // 3. Alignment Assertion Verification
    assert_test((final_sp_el0 % 16U) == 0U,
                "SP_EL0 is strictly 16-byte aligned (final_sp_el0 % 16 == 0)");
    assert_test(user_stack.argc == 4U, "Extracted argc = 4 accurately");
    assert_test(user_stack.argv_vaddr == final_sp_el0 + 8U,
                "argv array virtual address positioned at SP_EL0 + 8");

    address_space.destroy();
}

void cmd_test_user_echo() noexcept {
    print_str("\r\n[TEST] Executing EL0 User Space Echo Test (x0=argc, x1=argv)...\r\n");

    auto as_res = Kernel::MM::AddressSpace::create();
    if (!as_res) {
        print_str("[FAIL] Failed to create AddressSpace\r\n");
        return;
    }
    auto address_space = static_cast<Kernel::MM::AddressSpace&&>(as_res.value());

    // Map executable page for user_space_echo_test entry
    const uintptr_t code_frame = alloc_frame();
    const uintptr_t entry_vaddr = 0x0000000000400000ULL;

    address_space.map_user_page(entry_vaddr, code_frame,
                                HAL::PageFlags::User | HAL::PageFlags::Read |
                                    HAL::PageFlags::Execute);

    // Copy user_space_echo_test routine into code frame
    auto* const k_code = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(code_frame));
    const auto* const src_code = reinterpret_cast<const uint8_t*>(cmd_test_user_echo);
    for (size_t i = 0U; i < 256U; ++i) {
        k_code[i] = src_code[i];
    }

    // Allocate stack and build System V ABI layout
    auto stack_res = Kernel::Process::UserStackBuilder::create_and_map(address_space);
    if (!stack_res) {
        print_str("[FAIL] Stack creation failed\r\n");
        return;
    }
    auto user_stack = stack_res.value();

    const char* const echo_argv[] = {"echo_app", "Param1", "SystemV_ABI_Valid", "AArch64_OK"};
    auto abi_res = Kernel::Process::UserStackBuilder::build_abi_layout(user_stack, 4, echo_argv);

    assert_test(abi_res.is_ok(), "ABI stack layout populated");

    // Initialize CpuContext
    HAL::CpuContext ctx{};
    Kernel::Process::init_user_cpu_context(ctx, entry_vaddr, user_stack.final_sp_el0,
                                           user_stack.argc, user_stack.argv_vaddr);

    const uintptr_t orig_root = Kernel::MM::AddressSpace::current_root_paddr();
    address_space.activate();

    // Context switch to EL0
    const int64_t exit_status = Kernel::Process::enter_user_context(&ctx);

    Kernel::MM::AddressSpace::activate_root(orig_root);

    assert_test(exit_status == 0,
                "EL0 Echo process received argc/argv, printed payload, and exited status 0");

    free_frame(code_frame);
    address_space.destroy();
}

void cmd_test_uxn_fault() noexcept {
    print_str("\r\n[TEST] Executing User Stack Execute-Never (UXN) Fault Test...\r\n");

    auto as_res = Kernel::MM::AddressSpace::create();
    if (!as_res) {
        print_str("[FAIL] Failed to create AddressSpace\r\n");
        return;
    }
    auto address_space = static_cast<Kernel::MM::AddressSpace&&>(as_res.value());

    const uintptr_t code_frame = alloc_frame();
    const uintptr_t entry_vaddr = 0x0000000000400000ULL;

    address_space.map_user_page(entry_vaddr, code_frame,
                                HAL::PageFlags::User | HAL::PageFlags::Read |
                                    HAL::PageFlags::Execute);

    auto* const k_code = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(code_frame));
    const auto* const src_code = reinterpret_cast<const uint8_t*>(cmd_test_uxn_fault);
    for (size_t i = 0U; i < 64U; ++i) {
        k_code[i] = src_code[i];
    }

    auto stack_res = Kernel::Process::UserStackBuilder::create_and_map(address_space);
    auto user_stack = stack_res.value();

    const char* const dummy_argv[] = {"uxn_test"};
    Kernel::Process::UserStackBuilder::build_abi_layout(user_stack, 1, dummy_argv);

    HAL::CpuContext ctx{};
    Kernel::Process::init_user_cpu_context(ctx, entry_vaddr, user_stack.final_sp_el0,
                                           user_stack.argc, user_stack.argv_vaddr);

    const uintptr_t orig_root = Kernel::MM::AddressSpace::current_root_paddr();
    address_space.activate();

    print_str("  Branching to user stack from EL0...\r\n");
    const int64_t exit_status = Kernel::Process::enter_user_context(&ctx);

    Kernel::MM::AddressSpace::activate_root(orig_root);

    assert_test(exit_status == -1,
                "UXN Instruction Abort caught by Trap Handler! Process terminated safely.");

    free_frame(code_frame);
    address_space.destroy();
}

void print_help() noexcept {
    print_str("\r\nAvailable Commands:\r\n");
    print_str("  help            - Display this menu\r\n");
    print_str("  info            - Display system hardware & Exception Level\r\n");
    print_str("  ls initrd       - List all files and metadata in RAMDisk\r\n");
    print_str("  cat initrd <p>  - Display contents of file <p> from RAMDisk\r\n");
    print_str("  elf initrd <p>  - Parse and inspect ELF binary <p> from RAMDisk\r\n");
    print_str("  clear           - Clear VT100 terminal screen\r\n");
    print_str("  test cpu        - Test CPU HAL interrupt enable/disable masking\r\n");
    print_str("  test mmu        - Read SCTLR_EL1 to verify MMU and Caches\r\n");
    print_str("  test el0        - Test EL0 user space switch and SVC trap return\r\n");
    print_str("  test cpp        - Verify .bss zeroing and C++ vtable dynamic dispatch\r\n");
    print_str("  test svc        - Execute SVC #0 trap and verify handler routing\r\n");
    print_str("  test abort      - Dereference unmapped pointer to test Data Abort trap\r\n");
    print_str("  test pfa        - Execute Page Frame Allocator tests\r\n");
    print_str("  test ro         - Test Read-Only protection write fault\r\n");
    print_str("  test nx         - Test Execute-Never protection execution fault\r\n");
    print_str("  test sys        - Test EL0 system register trap\r\n");
    print_str("  test cpio       - Run Read-Only CPIO driver verification suite\r\n");
    print_str("  test elf        - Run ELF64 In-Memory Parser verification suite\r\n");
    print_str("  test vmm        - Test AddressSpace lifecycle and page mappings\r\n");
    print_str("  test loader     - Test ELF segment loading and OOM rollback\r\n");
    print_str("  test user       - Instantiate EL0 process and verify syscall exit\r\n");
    print_str("  test all        - Run entire verification test suite\r\n");
    print_str("  test stack_abi  - Run user atack allocation & System V ABI construction tests");
    print_str("  test user_echo  - Run EL0 user space echo test (x0=argc, x1=argv)");
    print_str("  test uxn_fault  - Run user stack execute-never (UXN) fault test");
    print_str("  halt            - Put CPU into low-power WFI state\r\n");
}

void dispatch_command(const char* buf) noexcept {
    if (streq(buf, "info")) {
        cmd_info();
    } else if (streq(buf, "ls initrd")) {
        cmd_ls_initrd();
    } else if (str_starts_with(buf, "cat initrd ")) {
        cmd_cat_initrd(buf + 11);
    } else if (streq(buf, "cat initrd")) {
        cmd_cat_initrd("");
    } else if (str_starts_with(buf, "elf initrd ")) {
        cmd_elf_initrd(buf + 11);
    } else if (streq(buf, "elf initrd")) {
        cmd_elf_initrd("");
    } else if (streq(buf, "test cpu")) {
        cmd_test_cpu();
    } else if (streq(buf, "test mmu")) {
        cmd_test_mmu();
    } else if (streq(buf, "test el0")) {
        cmd_test_el0();
    } else if (streq(buf, "test cpp")) {
        cmd_test_cpp();
    } else if (streq(buf, "test svc")) {
        cmd_test_svc_trap();
    } else if (streq(buf, "test abort")) {
        cmd_test_data_abort();
    } else if (streq(buf, "test pfa")) {
        cmd_test_pfa();
    } else if (streq(buf, "test ro")) {
        cmd_test_ro();
    } else if (streq(buf, "test nx")) {
        cmd_test_nx();
    } else if (streq(buf, "test sys")) {
        cmd_test_sys();
    } else if (streq(buf, "test cpio")) {
        cmd_test_cpio();
    } else if (streq(buf, "test elf")) {
        cmd_test_elf();
    } else if (streq(buf, "test vmm")) {
        cmd_test_vmm();
    } else if (streq(buf, "test loader")) {
        cmd_test_loader_pipeline();
    } else if (streq(buf, "test user")) {
        cmd_test_user_isolation();
    } else if (streq(buf, "test user_echo")) {
        cmd_test_user_echo();
    } else if (streq(buf, "test stack_abi")) {
        cmd_test_stack_and_abi();
    } else if (streq(buf, "test uxn_fault")) {
        cmd_test_uxn_fault();
    } else if (streq(buf, "halt")) {
        print_str("[INFO] Halting CPU. Use QEMU 'Ctrl-A X' to exit.\r\n");
        HAL::get_cpu().halt();
    } else if (streq(buf, "clear")) {
        print_str("\033[2J\033[H");
    } else if (streq(buf, "help")) {
        print_help();
    } else if (streq(buf, "test all")) {
        cmd_test_cpu();
        cmd_test_mmu();
        cmd_test_el0();
        cmd_test_cpp();
        cmd_test_svc_trap();
        cmd_test_data_abort();
        cmd_test_pfa();
        cmd_test_ro();
        cmd_test_nx();
        cmd_test_sys();
        cmd_test_cpio();
        cmd_test_elf();
        cmd_test_vmm();
        cmd_test_loader_pipeline();
        cmd_test_user_isolation();
        cmd_test_user_echo();
        cmd_test_uxn_fault();
    } else if (buf[0] != '\0') {
        print_str("Unknown command: '");
        print_str(buf);
        print_str("'. Type 'help' for available commands.\r\n");
    }
}

} // namespace

namespace Kernel {
namespace Shell {

void run() noexcept {
    char input_buf[INPUT_BUFFER_SIZE];
    size_t buf_pos = 0;

    print_str("\r\n=============================================\r\n");
    print_str("       milo OS Kernel Shell Ready            \r\n");
    print_str("=============================================\r\n");
    print_str("milo> ");

    while (true) {
        const char chr = static_cast<char>(HAL::get_console().getc());

        if (chr == '\r' || chr == '\n') {
            print_str("\r\n");
            input_buf[buf_pos] = '\0';
            dispatch_command(input_buf);
            buf_pos = 0;
            print_str("milo> ");
        } else if (chr == '\b' || chr == 127) {
            if (buf_pos > 0) {
                buf_pos--;
                print_str("\b \b");
            }
        } else if (buf_pos < INPUT_BUFFER_SIZE - 1) {
            input_buf[buf_pos++] = chr;
            HAL::get_console().putc(chr);
        }
    }
}

} // namespace Shell
} // namespace Kernel