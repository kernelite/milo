#pragma once

#include "core/traits.hpp"

#include <cstddef>
#include <cstdint>

// Standard fixed-width ELF64 types conforming to the System V ABI
using Elf64_Addr = uint64_t;
using Elf64_Off = uint64_t;
using Elf64_Half = uint16_t;
using Elf64_Word = uint32_t;
using Elf64_Sword = int32_t;
using Elf64_Xword = uint64_t;
using Elf64_Sxword = int64_t;

// Ident indices
inline constexpr size_t EI_MAG0 = 0;
inline constexpr size_t EI_MAG1 = 1;
inline constexpr size_t EI_MAG2 = 2;
inline constexpr size_t EI_MAG3 = 3;
inline constexpr size_t EI_CLASS = 4;
inline constexpr size_t EI_DATA = 5;
inline constexpr size_t EI_VERSION = 6;
inline constexpr size_t EI_OSABI = 7;
inline constexpr size_t EI_ABIVERSION = 8;
inline constexpr size_t EI_PAD = 9;
inline constexpr size_t EI_NIDENT = 16;

// Magic bytes
inline constexpr unsigned char ELFMAG0 = 0x7F;
inline constexpr unsigned char ELFMAG1 = 'E';
inline constexpr unsigned char ELFMAG2 = 'L';
inline constexpr unsigned char ELFMAG3 = 'F';

// Ident values
inline constexpr unsigned char ELFCLASSNONE = 0;
inline constexpr unsigned char ELFCLASS32 = 1;
inline constexpr unsigned char ELFCLASS64 = 2;

inline constexpr unsigned char ELFDATANONE = 0;
inline constexpr unsigned char ELFDATA2LSB = 1; // Little-Endian
inline constexpr unsigned char ELFDATA2MSB = 2; // Big-Endian

inline constexpr unsigned char EV_NONE = 0;
inline constexpr unsigned char EV_CURRENT = 1;

inline constexpr unsigned char ELFOSABI_NONE = 0;
inline constexpr unsigned char ELFOSABI_SYSV = 0;

// Binary types (e_type)
inline constexpr Elf64_Half ET_NONE = 0;
inline constexpr Elf64_Half ET_REL = 1;
inline constexpr Elf64_Half ET_EXEC = 2;
inline constexpr Elf64_Half ET_DYN = 3;
inline constexpr Elf64_Half ET_CORE = 4;

// Target architecture machines (e_machine)
inline constexpr Elf64_Half EM_NONE = 0;
inline constexpr Elf64_Half EM_ARM = 40;
inline constexpr Elf64_Half EM_X86_64 = 62;
inline constexpr Elf64_Half EM_AARCH64 = 183; // 0xB7
inline constexpr Elf64_Half EM_RISCV = 243;

// Segment types (p_type)
inline constexpr Elf64_Word PT_NULL = 0;
inline constexpr Elf64_Word PT_LOAD = 1;
inline constexpr Elf64_Word PT_DYNAMIC = 2;
inline constexpr Elf64_Word PT_INTERP = 3;
inline constexpr Elf64_Word PT_NOTE = 4;
inline constexpr Elf64_Word PT_SHLIB = 5;
inline constexpr Elf64_Word PT_PHDR = 6;
inline constexpr Elf64_Word PT_TLS = 7;
inline constexpr Elf64_Word PT_GNU_STACK = 0x6474E551;

// Segment permission flags (p_flags)
inline constexpr Elf64_Word PF_X = 1U << 0; // Execute
inline constexpr Elf64_Word PF_W = 1U << 1; // Write
inline constexpr Elf64_Word PF_R = 1U << 2; // Read

// 64-byte Execution Header
struct [[gnu::packed]] Elf64_Ehdr {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};
static_assert(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr must be exactly 64 bytes");

// 56-byte Program Header
struct [[gnu::packed]] Elf64_Phdr {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};
static_assert(sizeof(Elf64_Phdr) == 56, "Elf64_Phdr must be exactly 56 bytes");

// Explicit error model
enum class ElfStatus {
    Success = 0,
    BufferTooSmall,
    InvalidMagic,
    UnsupportedClass,      // e.g., 32-bit ELF
    UnsupportedEndianness, // e.g., Big-Endian
    UnsupportedVersion,
    UnsupportedMachine, // Not EM_AARCH64
    UnsupportedType,    // Neither ET_EXEC nor ET_DYN
    MalformedProgramHeaderTable,
    SegmentOutOfBounds,
    InvalidSegmentAlignment,
};

const char* elf_status_str(ElfStatus status) noexcept;

// Freestanding, zero-allocation Result type
template <typename T, typename E> class Result {
  public:
    constexpr Result(const T& val) noexcept : m_val(val), m_err{}, m_is_ok(true) {}
    constexpr Result(T&& val) noexcept : m_val(static_cast<T&&>(val)), m_err{}, m_is_ok(true) {}
    constexpr Result(E error) noexcept : m_val{}, m_err(error), m_is_ok(false) {}

    constexpr bool is_ok() const noexcept { return m_is_ok; }
    constexpr bool is_err() const noexcept { return !m_is_ok; }
    constexpr explicit operator bool() const noexcept { return m_is_ok; }

    constexpr const T& value() const noexcept { return m_val; }
    constexpr T& value() noexcept { return m_val; }

    constexpr const T* operator->() const noexcept { return &m_val; }
    constexpr T* operator->() noexcept { return &m_val; }

    constexpr const T& operator*() const noexcept { return m_val; }
    constexpr T& operator*() noexcept { return m_val; }

    constexpr E error() const noexcept { return m_err; }

    constexpr static Result ok(const T& val) noexcept { return Result(val); }
    constexpr static Result ok(T&& val) noexcept { return Result(static_cast<T&&>(val)); }
    constexpr static Result err(E error) noexcept { return Result(error); }

  private:
    T m_val{};
    E m_err{};
    bool m_is_ok{false};
};

// Extracted segment descriptor
struct Elf64Segment {
    uint32_t type{0};
    uint32_t flags{0};
    uint64_t offset{0};
    uint64_t vaddr{0};
    uint64_t filesz{0};
    uint64_t memsz{0};
    uint64_t align{0};
    const uint8_t* data{nullptr}; // Pointer to raw segment data within buffer

    // Zero-fill byte count (.bss) where memsz exceeds filesz
    constexpr uint64_t bss_size() const noexcept {
        return (memsz > filesz) ? (memsz - filesz) : 0ULL;
    }

    constexpr bool is_readable() const noexcept { return (flags & PF_R) != 0U; }

    constexpr bool is_writable() const noexcept { return (flags & PF_W) != 0U; }

    constexpr bool is_executable() const noexcept { return (flags & PF_X) != 0U; }
};

// Non-owning in-memory ELF64 view
class Elf64View {
  public:
    constexpr Elf64View() = default;

    // Primary parsing and validation entry point
    static Result<Elf64View, ElfStatus> parse(const uint8_t* data, size_t size) noexcept;

    static Result<Elf64View, ElfStatus> parse(const void* data, size_t size) noexcept {
        return parse(static_cast<const uint8_t*>(data), size);
    }

    constexpr uint64_t entry_point() const noexcept {
        return (m_ehdr != nullptr) ? m_ehdr->e_entry : 0ULL;
    }

    constexpr size_t segment_count() const noexcept {
        return (m_ehdr != nullptr) ? m_ehdr->e_phnum : 0U;
    }

    constexpr uint16_t type() const noexcept {
        return (m_ehdr != nullptr) ? m_ehdr->e_type : static_cast<uint16_t>(ET_NONE);
    }

    constexpr bool is_executable() const noexcept { return type() == ET_EXEC; }

    constexpr bool is_position_independent() const noexcept { return type() == ET_DYN; }

    constexpr const Elf64_Ehdr& header() const noexcept { return *m_ehdr; }

    constexpr const Elf64_Phdr* program_headers() const noexcept { return m_phdrs; }

    constexpr const uint8_t* buffer() const noexcept { return m_data; }

    constexpr size_t buffer_size() const noexcept { return m_size; }

    size_t load_segment_count() const noexcept {
        if (m_ehdr == nullptr || m_phdrs == nullptr) {
            return 0U;
        }
        size_t count = 0U;
        for (size_t i = 0U; i < m_ehdr->e_phnum; ++i) {
            if (m_phdrs[i].p_type == PT_LOAD) {
                count++;
            }
        }
        return count;
    }

    // Zero-allocation iteration over PT_LOAD segments
    template <typename Func> void for_each_load_segment(Func&& callback) const {
        if (m_ehdr == nullptr || m_phdrs == nullptr) {
            return;
        }

        for (size_t i = 0U; i < m_ehdr->e_phnum; ++i) {
            const auto& phdr = m_phdrs[i];
            if (phdr.p_type == PT_LOAD) {
                const Elf64Segment segment{
                    .type = phdr.p_type,
                    .flags = phdr.p_flags,
                    .offset = phdr.p_offset,
                    .vaddr = phdr.p_vaddr,
                    .filesz = phdr.p_filesz,
                    .memsz = phdr.p_memsz,
                    .align = phdr.p_align,
                    .data = (phdr.p_filesz > 0 && m_data != nullptr) ? (m_data + phdr.p_offset)
                                                                     : nullptr,
                };

                if constexpr (Kernel::Traits::is_bool<decltype(callback(segment))>::value) {
                    if (!callback(segment)) {
                        break;
                    }
                } else {
                    callback(segment);
                }
            }
        }
    }

    // Zero-allocation iteration over all program segments
    template <typename Func> void for_each_segment(Func&& callback) const {
        if (m_ehdr == nullptr || m_phdrs == nullptr) {
            return;
        }

        for (size_t i = 0U; i < m_ehdr->e_phnum; ++i) {
            const auto& phdr = m_phdrs[i];
            const Elf64Segment segment{
                .type = phdr.p_type,
                .flags = phdr.p_flags,
                .offset = phdr.p_offset,
                .vaddr = phdr.p_vaddr,
                .filesz = phdr.p_filesz,
                .memsz = phdr.p_memsz,
                .align = phdr.p_align,
                .data =
                    (phdr.p_filesz > 0 && m_data != nullptr) ? (m_data + phdr.p_offset) : nullptr,
            };

            if constexpr (Kernel::Traits::is_bool<decltype(callback(segment))>::value) {
                if (!callback(segment)) {
                    break;
                }
            } else {
                callback(segment);
            }
        }
    }

  private:
    constexpr Elf64View(const uint8_t* data, size_t size, const Elf64_Ehdr* ehdr,
                        const Elf64_Phdr* phdrs) noexcept
        : m_data(data), m_size(size), m_ehdr(ehdr), m_phdrs(phdrs) {}

    const uint8_t* m_data{nullptr};
    size_t m_size{0};
    const Elf64_Ehdr* m_ehdr{nullptr};
    const Elf64_Phdr* m_phdrs{nullptr};
};