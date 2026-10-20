#include "loader/elf64.hpp"

const char* elf_status_str(ElfStatus status) noexcept {
    switch (status) {
    case ElfStatus::Success:
        return "Success";
    case ElfStatus::BufferTooSmall:
        return "BufferTooSmall";
    case ElfStatus::InvalidMagic:
        return "InvalidMagic";
    case ElfStatus::UnsupportedClass:
        return "UnsupportedClass";
    case ElfStatus::UnsupportedEndianness:
        return "UnsupportedEndianness";
    case ElfStatus::UnsupportedVersion:
        return "UnsupportedVersion";
    case ElfStatus::UnsupportedMachine:
        return "UnsupportedMachine";
    case ElfStatus::UnsupportedType:
        return "UnsupportedType";
    case ElfStatus::MalformedProgramHeaderTable:
        return "MalformedProgramHeaderTable";
    case ElfStatus::SegmentOutOfBounds:
        return "SegmentOutOfBounds";
    case ElfStatus::InvalidSegmentAlignment:
        return "InvalidSegmentAlignment";
    default:
        return "UnknownError";
    }
}

Result<Elf64View, ElfStatus> Elf64View::parse(const uint8_t* data, size_t size) noexcept {
    // 1. Validate minimum buffer size
    if (data == nullptr || size < sizeof(Elf64_Ehdr)) {
        return ElfStatus::BufferTooSmall;
    }

    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);

    // 2. Validate magic bytes (0x7F, 'E', 'L', 'F')
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return ElfStatus::InvalidMagic;
    }

    // 3. Enforce 64-bit architecture
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return ElfStatus::UnsupportedClass;
    }

    // 4. Enforce Little-Endian byte order
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return ElfStatus::UnsupportedEndianness;
    }

    // 5. Enforce current ELF version
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
        return ElfStatus::UnsupportedVersion;
    }

    // 6. Enforce AArch64 target architecture
    if (ehdr->e_machine != EM_AARCH64) {
        return ElfStatus::UnsupportedMachine;
    }

    // 7. Enforce executable or position-independent executable type
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        return ElfStatus::UnsupportedType;
    }

    // 8. Validate execution header size
    if (ehdr->e_ehsize != sizeof(Elf64_Ehdr)) {
        return ElfStatus::MalformedProgramHeaderTable;
    }

    // Executable binaries must contain at least one program header
    if (ehdr->e_phnum == 0U) {
        return ElfStatus::MalformedProgramHeaderTable;
    }

    // Enforce standard 56-byte Program Header entry size
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        return ElfStatus::MalformedProgramHeaderTable;
    }

    // Program header table bounds and overflow protection
    const uint64_t ph_table_size = static_cast<uint64_t>(ehdr->e_phnum) * sizeof(Elf64_Phdr);
    if (ehdr->e_phoff > size || ph_table_size > size - ehdr->e_phoff) {
        return ElfStatus::MalformedProgramHeaderTable;
    }

    const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);

    // 9. Validate each PT_LOAD segment
    for (size_t i = 0U; i < ehdr->e_phnum; ++i) {
        const auto& phdr = phdrs[i];
        if (phdr.p_type == PT_LOAD) {
            // Memory vs. File Size check: filesz <= memsz (difference represents .bss)
            if (phdr.p_filesz > phdr.p_memsz) {
                return ElfStatus::SegmentOutOfBounds;
            }

            // Buffer containment with arithmetic overflow protection
            if (phdr.p_offset > size || phdr.p_filesz > size - phdr.p_offset) {
                return ElfStatus::SegmentOutOfBounds;
            }

            // Alignment: p_align must be 0 or a power of 2
            if (phdr.p_align > 1U) {
                if ((phdr.p_align & (phdr.p_align - 1U)) != 0U) {
                    return ElfStatus::InvalidSegmentAlignment;
                }
                // If non-zero, verify congruent offsets modulo alignment
                if ((phdr.p_vaddr % phdr.p_align) != (phdr.p_offset % phdr.p_align)) {
                    return ElfStatus::InvalidSegmentAlignment;
                }
            }
        }
    }

    return Elf64View(data, size, ehdr, phdrs);
}