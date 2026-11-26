#include "fs/Vfs.hpp"
#include "fs/devfs/devfs.hpp"
#include "fs/rootfs/rootfs.hpp"
#include "ipc/Pipe.hpp"
#include "mocks/host_hal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <snitch/snitch.hpp>
#include <string_view>

namespace {

using namespace Kernel::FS;

class CpioBuilder {
  public:
    void add_entry(std::string_view name, uint32_t mode, std::string_view payload) {
        const size_t name_size = name.size() + 1U;
        const size_t payload_size = payload.size();

        write_str("070701");
        write_hex8(m_next_ino++);
        write_hex8(mode);
        write_hex8(0U);
        write_hex8(0U);
        write_hex8(((mode & kModeTypeMask) == kModeDir) ? 2U : 1U);
        write_hex8(0U);
        write_hex8(static_cast<uint32_t>(payload_size));
        write_hex8(0U);
        write_hex8(0U);
        write_hex8(0U);
        write_hex8(0U);
        write_hex8(static_cast<uint32_t>(name_size));
        write_hex8(0U);

        write_bytes(name.data(), name.size());
        write_byte(0U);
        align4();

        if (payload_size > 0U) {
            write_bytes(payload.data(), payload_size);
            align4();
        }
    }

    void finish() { add_entry("TRAILER!!!", 0U, ""); }

    [[nodiscard]] const uint8_t* data() const noexcept { return m_buf.data(); }
    [[nodiscard]] size_t size() const noexcept { return m_pos; }

  private:
    void write_byte(uint8_t val) {
        REQUIRE(m_pos < m_buf.size());
        m_buf[m_pos++] = val;
    }

    void write_bytes(const void* src, size_t len) {
        REQUIRE(m_pos + len <= m_buf.size());
        std::memcpy(m_buf.data() + m_pos, src, len);
        m_pos += len;
    }

    void write_str(std::string_view str) { write_bytes(str.data(), str.size()); }

    void write_hex8(uint32_t val) {
        constexpr char kHex[] = "0123456789abcdef";
        char hex[8]{};
        for (int idx = 7; idx >= 0; --idx) {
            hex[idx] = kHex[val & 0xFU];
            val >>= 4U;
        }
        write_bytes(hex, sizeof(hex));
    }

    void align4() {
        while ((m_pos & 3U) != 0U) {
            write_byte(0U);
        }
    }

    std::array<uint8_t, 8192> m_buf{};
    size_t m_pos{0U};
    uint32_t m_next_ino{10U};
};

} // namespace

TEST_CASE("rootfs populates VFS hierarchy from CPIO and enforces read-only semantics", "[rootfs]") {
    HostTest::reset_host_hal();
    vfs_reset();
    rootfs_reset();
    devfs_reset();

    CpioBuilder builder;
    builder.add_entry(".", kModeDir | 0755U, "");
    builder.add_entry("init.txt", kModeReg | 0644U, "milo-initrd-v1\n");
    builder.add_entry("bin", kModeDir | 0755U, "");
    builder.add_entry("bin/busybox", kModeReg | 0755U, "ELF_PAYLOAD_DATA");
    builder.add_entry("bin/sh", kModeLnk | 0777U, "busybox");
    builder.add_entry("etc/nested/config.cfg", kModeReg | 0644U, "key=value");
    builder.finish();

    REQUIRE(rootfs_mount_cpio(builder.data(), builder.size()) == kOk);

    SECTION("resolves nested paths, symlinks, and synthesized directories") {
        Dentry* resolved = nullptr;
        REQUIRE(vfs_namei("/init.txt", nullptr, &resolved) == kOk);
        REQUIRE(resolved->inode->size == 15U);
        dentry_put(resolved);

        REQUIRE(vfs_namei("/etc/nested/config.cfg", nullptr, &resolved) == kOk);
        REQUIRE(resolved->inode->size == 9U);
        dentry_put(resolved);

        REQUIRE(vfs_namei("/bin/sh", nullptr, &resolved, true) == kOk);
        REQUIRE(resolved->name_view() == "busybox");
        dentry_put(resolved);

        REQUIRE(vfs_namei("/bin/sh", nullptr, &resolved, false) == kOk);
        REQUIRE(resolved->inode->is_symlink());
        dentry_put(resolved);

        char link_target[32]{};
        size_t link_len = 0U;
        REQUIRE(vfs_readlink("/bin/sh", nullptr, link_target, sizeof(link_target), &link_len) ==
                kOk);
        REQUIRE(std::string_view(link_target, link_len) == "busybox");
    }

    SECTION("supports file read, lseek modes, and directory readdir") {
        File* file = nullptr;
        REQUIRE(vfs_open("/bin/busybox", kOpenReadOnly, 0U, &file) == kOk);

        char buf[16]{};
        REQUIRE(vfs_read(file, buf, 4U) == 4);
        REQUIRE(std::string_view(buf, 4) == "ELF_");

        REQUIRE(vfs_lseek(file, 8, SeekWhence::kCur) == 12);
        REQUIRE(vfs_read(file, buf, 4U) == 4);
        REQUIRE(std::string_view(buf, 4) == "DATA");
        REQUIRE(vfs_read(file, buf, 4U) == 0);

        REQUIRE(vfs_lseek(file, -4, SeekWhence::kEnd) == 12);
        REQUIRE(vfs_lseek(file, 0, SeekWhence::kSet) == 0);
        REQUIRE(vfs_lseek(file, -1, SeekWhence::kSet) == kEinval);
        REQUIRE(vfs_close(file) == kOk);

        File* dir_file = nullptr;
        REQUIRE(vfs_open("/bin", kOpenReadOnly | kOpenDirectory, 0U, &dir_file) == kOk);
        DirEntryInfo entry{};
        size_t count = 0U;
        while (vfs_readdir(dir_file, &entry) == kOk) {
            ++count;
        }
        REQUIRE(count == 2U);
        REQUIRE(vfs_close(dir_file) == kOk);
    }

    SECTION("rejects all mutating operations with EROFS") {
        File* file = nullptr;
        REQUIRE(vfs_open("/init.txt", kOpenWriteOnly, 0U, &file) == kErofs);
        REQUIRE(vfs_open("/newfile.txt", kOpenCreate | kOpenWriteOnly, 0644U, &file) == kErofs);
        REQUIRE(vfs_mkdir("/newdir", 0755U) == kErofs);
        REQUIRE(vfs_unlink("/init.txt") == kErofs);
        REQUIRE(vfs_rmdir("/bin") == kErofs);
        REQUIRE(vfs_rename("/init.txt", "/other.txt") == kErofs);

        REQUIRE(vfs_open("/init.txt", kOpenReadOnly, 0U, &file) == kOk);
        REQUIRE(file->f_ops->write(file, "x", 1U, &file->offset) == kErofs);
        REQUIRE(vfs_close(file) == kOk);
    }
}

TEST_CASE("devfs mounts /dev/console and /dev/null over VFS", "[devfs]") {
    HostTest::reset_host_hal();
    vfs_reset();
    rootfs_reset();
    devfs_reset();

    REQUIRE(rootfs_mount_cpio(nullptr, 0U) == kOk);
    REQUIRE(devfs_init_and_mount() == kOk);

    File* console_file = nullptr;
    REQUIRE(vfs_open("/dev/console", kOpenWriteOnly, 0U, &console_file) == kOk);
    REQUIRE(vfs_write(console_file, "uart-ok", 7U) == 7);
    REQUIRE(vfs_lseek(console_file, 0, SeekWhence::kSet) == kEspipe);
    REQUIRE(vfs_close(console_file) == kOk);
    REQUIRE(std::string_view(HostTest::get_captured_console_output()) == "uart-ok");

    File* null_file = nullptr;
    REQUIRE(vfs_open("/dev/null", kOpenReadWrite, 0U, &null_file) == kOk);
    REQUIRE(vfs_write(null_file, "discard", 7U) == 7);
    char buf[8]{};
    REQUIRE(vfs_read(null_file, buf, sizeof(buf)) == 0);
    REQUIRE(vfs_lseek(null_file, 123, SeekWhence::kSet) == 0);
    REQUIRE(vfs_close(null_file) == kOk);
}

TEST_CASE("VFS-backed IPC pipes transfer ring buffer data and propagate EOF", "[ipc][pipe]") {
    HostTest::reset_host_hal();
    vfs_reset();

    File* read_end = nullptr;
    File* write_end = nullptr;
    REQUIRE(Kernel::IPC::create_pipe_files(&read_end, &write_end, kOpenNonBlock) == kOk);
    REQUIRE(read_end != nullptr);
    REQUIRE(write_end != nullptr);
    REQUIRE(read_end->inode->is_fifo());

    char buf[32]{};
    REQUIRE(vfs_read(read_end, buf, sizeof(buf)) == kEagain);
    REQUIRE(vfs_lseek(read_end, 0, SeekWhence::kSet) == kEspipe);

    const std::string_view payload = "hello-vfs-pipe";
    REQUIRE(vfs_write(write_end, payload.data(), payload.size()) ==
            static_cast<int64_t>(payload.size()));
    REQUIRE(vfs_read(read_end, buf, sizeof(buf)) == static_cast<int64_t>(payload.size()));
    REQUIRE(std::string_view(buf, payload.size()) == payload);

    REQUIRE(vfs_close(write_end) == kOk);
    REQUIRE(vfs_read(read_end, buf, sizeof(buf)) == 0);
    REQUIRE(vfs_close(read_end) == kOk);
}