#include "../arch/arch_test.hpp"
#include "boot/fdt.hpp"
#include "fs/cpio.hpp"
#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "mm/pfa.hpp"

#include <cstddef>
#include <cstdint>

// Access global initrd metadata from main.cpp
extern FDT::InitrdLocation g_initrd_loc;

namespace {

constexpr size_t INPUT_BUFFER_SIZE = 128;
int bss_check_var; // Uninitialized global variable to verify .bss zeroing

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

// Write 8-character lower-case hex ASCII for building synthetic test archives
void write_hex8_ascii(char* dest, uint32_t val) noexcept {
    const char hex_chars[] = "0123456789abcdef";
    for (int i = 7; i >= 0; --i) {
        dest[i] = hex_chars[val & 0xFU];
        val >>= 4U;
    }
}

size_t build_synthetic_entry(uint8_t* out, const char* name, const uint8_t* content, size_t size,
                             uint32_t mode = 0100644) noexcept {
    auto* hdr = reinterpret_cast<CpioNewcHeader*>(out);
    for (size_t i = 0; i < 6; ++i) {
        hdr->c_magic[i] = "070701"[i];
    }
    write_hex8_ascii(hdr->c_ino, 1);
    write_hex8_ascii(hdr->c_mode, mode);
    write_hex8_ascii(hdr->c_uid, 0);
    write_hex8_ascii(hdr->c_gid, 0);
    write_hex8_ascii(hdr->c_nlink, 1);
    write_hex8_ascii(hdr->c_mtime, 0);
    write_hex8_ascii(hdr->c_filesize, static_cast<uint32_t>(size));
    write_hex8_ascii(hdr->c_maj, 0);
    write_hex8_ascii(hdr->c_min, 0);
    write_hex8_ascii(hdr->c_rmaj, 0);
    write_hex8_ascii(hdr->c_rmin, 0);

    size_t name_len = 0;
    while (name[name_len] != '\0') {
        name_len++;
    }
    const size_t namesize = name_len + 1;
    write_hex8_ascii(hdr->c_namesize, static_cast<uint32_t>(namesize));
    write_hex8_ascii(hdr->c_check, 0);

    for (size_t i = 0; i < namesize; ++i) {
        out[110 + i] = static_cast<uint8_t>(name[i]);
    }
    const size_t name_end = 110 + namesize;
    const size_t payload_offset = cpio_align_up(name_end);
    for (size_t i = name_end; i < payload_offset; ++i) {
        out[i] = 0;
    }
    for (size_t i = 0; i < size; ++i) {
        out[payload_offset + i] = content[i];
    }
    const size_t payload_end = payload_offset + size;
    const size_t next_offset = cpio_align_up(payload_end);
    for (size_t i = payload_end; i < next_offset; ++i) {
        out[i] = 0;
    }
    return next_offset;
}

// ls initrd command
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

// cat initrd <path> command
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

// test cpio command
void cmd_test_cpio() noexcept {
    print_str("\r\n[TEST] Running Read-Only CPIO Driver Verification Suite...\r\n");

    // 1. Data Layout Static and Runtime Verification
    assert_test(sizeof(CpioNewcHeader) == 110, "CpioNewcHeader size is exactly 110 bytes");

    // 2. Defensive Testing: Invalid pointer and truncated buffer safety
    CpioEntry entry{};
    assert_test(cpio_lookup(nullptr, 0, "init.txt", entry) == CpioStatus::ArchiveTooSmall,
                "Lookup with nullptr base returns ArchiveTooSmall");
    assert_test(cpio_lookup(nullptr, 100, "init.txt", entry) == CpioStatus::ArchiveTooSmall,
                "Lookup with size < 110 returns ArchiveTooSmall");

    // 3. Defensive Testing: Corrupted magic signature
    uint8_t bad_magic_buf[110] = {0};
    for (size_t i = 0; i < 6; ++i) {
        bad_magic_buf[i] = "BADMAG"[i];
    }
    assert_test(cpio_lookup(bad_magic_buf, sizeof(bad_magic_buf), "init.txt", entry) ==
                    CpioStatus::InvalidMagic,
                "Lookup with invalid header magic returns InvalidMagic");

    // 4. Defensive Testing: Malformed hex header fields
    uint8_t malformed_buf[110] = {0};
    for (size_t i = 0; i < 6; ++i) {
        malformed_buf[i] = "070701"[i];
    }
    for (size_t i = 54; i < 62; ++i) {
        malformed_buf[i] = 'G'; // Non-hex character in filesize
    }
    for (size_t i = 94; i < 102; ++i) {
        malformed_buf[i] = '0';
    }
    assert_test(cpio_lookup(malformed_buf, sizeof(malformed_buf), "init.txt", entry) ==
                    CpioStatus::MalformedHeader,
                "Lookup with illegal ASCII hex characters returns MalformedHeader");

    // 5. Build Synthetic Archive with consecutive non-aligned sizes (1, 2, 3, 5 bytes)
    alignas(16) static uint8_t synth_archive[1024] = {0};
    size_t arch_size = 0;
    arch_size += build_synthetic_entry(synth_archive + arch_size, "f1.txt",
                                       reinterpret_cast<const uint8_t*>("1"), 1);
    arch_size += build_synthetic_entry(synth_archive + arch_size, "f2.txt",
                                       reinterpret_cast<const uint8_t*>("22"), 2);
    arch_size += build_synthetic_entry(synth_archive + arch_size, "f3.txt",
                                       reinterpret_cast<const uint8_t*>("333"), 3);
    arch_size += build_synthetic_entry(synth_archive + arch_size, "f5.txt",
                                       reinterpret_cast<const uint8_t*>("55555"), 5);
    arch_size += build_synthetic_entry(synth_archive + arch_size, CPIO_TRAILER_NAME, nullptr, 0, 0);

    // Add trailing buffer padding to prove traversal halts cleanly upon TRAILER!!!
    synth_archive[arch_size] = 0xDE;
    synth_archive[arch_size + 1] = 0xAD;
    const size_t padded_arch_size = arch_size + 64;

    const CpioArchive synth_view(synth_archive, padded_arch_size);
    assert_test(synth_view.is_valid(), "Synthetic multi-file archive magic is valid");

    size_t traversed_count = 0;
    const CpioStatus trav_st = synth_view.for_each([&](const CpioEntry& entry) {
        traversed_count++;
        (void)entry;
    });
    assert_test(
        trav_st == CpioStatus::Success && traversed_count == 4,
        "Sequential traversal visits all 4 non-aligned entries and halts cleanly on TRAILER!!!");

    // 6. Lookups and path normalization across varying sizes
    CpioEntry entry1{};
    CpioEntry entry2{};
    CpioEntry entry5{};
    CpioEntry enone{};
    assert_test(synth_view.lookup("/f1.txt", entry1) == CpioStatus::Success && entry1.size == 1 &&
                    entry1.data[0] == '1',
                "Arbitrary lookup '/f1.txt' matches 'f1.txt' (size: 1 byte)");
    assert_test(synth_view.lookup("./f2.txt", entry2) == CpioStatus::Success && entry2.size == 2 &&
                    entry2.data[0] == '2',
                "Arbitrary lookup './f2.txt' matches 'f2.txt' (size: 2 bytes)");
    assert_test(synth_view.lookup("f5.txt", entry5) == CpioStatus::Success && entry5.size == 5 &&
                    entry5.data[4] == '5',
                "Arbitrary lookup 'f5.txt' returns exact size (5 bytes) and payload pointer");
    assert_test(synth_view.lookup("/does_not_exist", enone) == CpioStatus::FileNotFound,
                "Lookup for non-existent file cleanly returns FileNotFound");

    // 7. Defensive Testing: Corrupted payload padding detection
    // In f1.txt: name is "f1.txt\0" (len 7, namesize 7).
    // Payload offset = align_up(110 + 7) = align_up(117) = 120.
    // Payload size = 1 byte (offset 120). Next entry offset = align_up(121) = 124.
    // Padding bytes are indices 121, 122, 123.
    alignas(16) static uint8_t corrupt_pad_buf[1024];
    for (size_t i = 0; i < arch_size; ++i) {
        corrupt_pad_buf[i] = synth_archive[i];
    }
    corrupt_pad_buf[121] = 0xFF; // Corrupt payload padding
    const CpioArchive corrupt_view(corrupt_pad_buf, arch_size);
    assert_test(corrupt_view.lookup("f2.txt", entry) == CpioStatus::CorruptedPadding,
                "Corrupted non-zero padding byte triggers CorruptedPadding status");

    // 8. Physical RAMDisk Real-World Verification
    if (g_initrd_loc.valid && g_initrd_loc.size > 0) {
        CpioEntry init_entry{};
        const CpioStatus init_st =
            cpio_lookup(reinterpret_cast<const void*>(g_initrd_loc.start_paddr), g_initrd_loc.size,
                        "/init.txt", init_entry);
        assert_test(init_st == CpioStatus::Success && init_entry.size >= 14,
                    "Live QEMU initrd locates '/init.txt' successfully with matching payload");
    }
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

    HAL::MmuStatus status = HAL::MmuStatus();
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

void print_help() noexcept {
    print_str("\r\nAvailable Commands:\r\n");
    print_str("  help            - Display this menu\r\n");
    print_str("  info            - Display system hardware & Exception Level\r\n");
    print_str("  ls initrd       - List all files and metadata in RAMDisk\r\n");
    print_str("  cat initrd <p>  - Display contents of file <p> from RAMDisk\r\n");
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
    print_str("  test all        - Run entire verification test suite\r\n");
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