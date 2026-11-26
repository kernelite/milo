#include "fs/Vfs.hpp"

#include <array>
#include <cstring>
#include <snitch/snitch.hpp>
#include <string_view>

namespace {

using namespace Kernel::FS;

// Static, zero-allocation mock filesystem backing for host VFS tests.
struct MockFileData {
    std::array<uint8_t, 512> bytes{};
    uint64_t next_ino{100};
};

struct MockFsInstance {
    Superblock* sb{nullptr};
    MockFileData data{};
};

int64_t mock_read(File* file, void* buf, size_t count, uint64_t* offset) noexcept {
    if (file == nullptr || file->get_inode() == nullptr || offset == nullptr) {
        return kEinval;
    }
    auto* data = static_cast<MockFileData*>(file->get_inode()->get_fs_private());
    if (data == nullptr) {
        return kEio;
    }
    if (*offset >= file->get_inode()->get_size()) {
        return 0;
    }
    const size_t avail = static_cast<size_t>(file->get_inode()->get_size() - *offset);
    const size_t to_copy = (count < avail) ? count : avail;
    std::memcpy(buf, data->bytes.data() + *offset, to_copy);
    *offset += to_copy;
    return static_cast<int64_t>(to_copy);
}

int64_t mock_write(File* file, const void* buf, size_t count, uint64_t* offset) noexcept {
    if (file == nullptr || file->get_inode() == nullptr || offset == nullptr) {
        return kEinval;
    }
    auto* data = static_cast<MockFileData*>(file->get_inode()->get_fs_private());
    if (data == nullptr) {
        return kEio;
    }
    if (*offset >= data->bytes.size()) {
        return kEnospc;
    }
    const size_t space = data->bytes.size() - static_cast<size_t>(*offset);
    const size_t to_copy = (count < space) ? count : space;
    std::memcpy(data->bytes.data() + *offset, buf, to_copy);
    *offset += to_copy;
    if (*offset > file->get_inode()->get_size()) {
        file->get_inode()->set_size(*offset);
    }
    return static_cast<int64_t>(to_copy);
}

int32_t mock_readdir(File* file, DirEntryInfo* out_entry) noexcept {
    if (file == nullptr || file->get_dentry() == nullptr || out_entry == nullptr) {
        return kEinval;
    }
    uint64_t idx = 0;
    for (Dentry* child = file->get_dentry()->get_first_child(); child != nullptr;
         child = child->get_next_sibling()) {
        if (!child->is_active() || child->get_inode() == nullptr) {
            continue;
        }
        if (idx == file->get_offset()) {
            out_entry->ino = child->get_inode()->get_ino();
            out_entry->type = child->get_inode()->get_mode() & kModeTypeMask;
            std::memset(out_entry->name, 0, sizeof(out_entry->name));
            std::memcpy(out_entry->name, child->name_view().data(), child->name_view().size());
            file->set_offset(file->get_offset() + 1U);
            return kOk;
        }
        ++idx;
    }
    return kEnoent;
}

int32_t mock_truncate(Inode* inode, uint64_t new_size) noexcept {
    if (inode == nullptr) {
        return kEinval;
    }
    auto* data = static_cast<MockFileData*>(inode->get_fs_private());
    if (data != nullptr && new_size > data->bytes.size()) {
        return kEnospc;
    }
    if (data != nullptr && new_size < inode->get_size()) {
        std::memset(data->bytes.data() + new_size, 0,
                    static_cast<size_t>(inode->get_size() - new_size));
    }
    inode->set_size(new_size);
    return kOk;
}

extern const InodeOperations kMockInodeOps;
extern const FileOperations kMockFileOps;

int32_t mock_create(Inode* dir, Dentry* target, uint32_t mode) noexcept {
    if (dir == nullptr || target == nullptr || dir->get_sb() == nullptr) {
        return kEinval;
    }
    auto* data = static_cast<MockFileData*>(dir->get_fs_private());
    Inode* inode = vfs_alloc_inode(dir->get_sb());
    if (inode == nullptr) {
        return kEnomem;
    }
    inode->set_ino((data != nullptr) ? data->next_ino++ : 2);
    inode->set_mode(mode);
    inode->set_i_ops(&kMockInodeOps);
    inode->set_f_ops(&kMockFileOps);
    inode->set_fs_private(data);
    target->bind_inode(inode);
    return kOk;
}

int32_t mock_mkdir(Inode* dir, Dentry* target, uint32_t mode) noexcept {
    if (dir == nullptr || target == nullptr || dir->get_sb() == nullptr) {
        return kEinval;
    }
    auto* data = static_cast<MockFileData*>(dir->get_fs_private());
    Inode* inode = vfs_alloc_inode(dir->get_sb());
    if (inode == nullptr) {
        return kEnomem;
    }
    inode->set_ino((data != nullptr) ? data->next_ino++ : 2);
    inode->set_mode(mode);
    inode->set_nlink(2);
    inode->set_i_ops(&kMockInodeOps);
    inode->set_f_ops(&kMockFileOps);
    inode->set_fs_private(data);
    target->bind_inode(inode);
    dir->set_nlink(dir->get_nlink() + 1U);
    return kOk;
}

int32_t mock_unlink(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || target->get_inode() == nullptr) {
        return kEinval;
    }
    if (target->get_inode()->get_nlink() > 0U) {
        target->get_inode()->set_nlink(target->get_inode()->get_nlink() - 1U);
    }
    return kOk;
}

int32_t mock_rmdir(Inode* dir, Dentry* target) noexcept {
    if (dir == nullptr || target == nullptr || target->get_inode() == nullptr) {
        return kEinval;
    }
    if (dir->get_nlink() > 0U) {
        dir->set_nlink(dir->get_nlink() - 1U);
    }
    target->get_inode()->set_nlink(0U);
    return kOk;
}

int32_t mock_rename(Inode* /*old_dir*/, Dentry* /*old_dentry*/, Inode* /*new_dir*/,
                    Dentry* new_dentry) noexcept {
    if (new_dentry != nullptr && new_dentry->get_inode() != nullptr &&
        new_dentry->get_inode()->get_nlink() > 0U) {
        new_dentry->get_inode()->set_nlink(new_dentry->get_inode()->get_nlink() - 1U);
    }
    return kOk;
}

int32_t mock_sync_fs(Superblock* /*sb*/) noexcept {
    return kOk;
}

const SuperblockOperations kMockSbOps{
    .sync_fs = mock_sync_fs,
    .alloc_inode = nullptr,
    .destroy_inode = nullptr,
};

const InodeOperations kMockInodeOps{
    .lookup = nullptr,
    .create = mock_create,
    .mkdir = mock_mkdir,
    .unlink = mock_unlink,
    .rmdir = mock_rmdir,
    .rename = mock_rename,
    .truncate = mock_truncate,
};

const FileOperations kMockFileOps{
    .read = mock_read,
    .write = mock_write,
    .open = nullptr,
    .release = nullptr,
    .lseek = nullptr,
    .ioctl = nullptr,
    .fsync = nullptr,
    .readdir = mock_readdir,
};

MockFsInstance init_mock_fs(const char* name, uint64_t sb_flags = 0) {
    MockFsInstance instance{};
    instance.sb = vfs_alloc_superblock();
    REQUIRE(instance.sb != nullptr);
    instance.sb->set_fs_type(name);
    instance.sb->set_flags(sb_flags);
    instance.sb->set_ops(&kMockSbOps);

    Inode* root_ino = vfs_alloc_inode(instance.sb);
    REQUIRE(root_ino != nullptr);
    root_ino->set_ino(1);
    root_ino->set_mode(kModeDir | 0755U);
    root_ino->set_nlink(2);
    root_ino->set_i_ops(&kMockInodeOps);
    root_ino->set_f_ops(&kMockFileOps);
    root_ino->set_fs_private(&instance.data);

    Dentry* root_dentry = vfs_alloc_dentry(instance.sb, nullptr, "/");
    REQUIRE(root_dentry != nullptr);
    root_dentry->bind_inode(root_ino);
    instance.sb->set_root(root_dentry);
    return instance;
}

void seed_file(MockFsInstance& instance, StringView path, StringView content) {
    File* file = nullptr;
    REQUIRE(vfs_open(path, kOpenCreate | kOpenWriteOnly | kOpenTrunc, 0644U, &file) == kOk);
    REQUIRE(file != nullptr);
    if (!content.empty()) {
        if (file->get_inode()->get_fs_private() == nullptr) {
            file->get_inode()->set_fs_private(&instance.data);
        }
        REQUIRE(vfs_write(file, content.data(), content.size()) ==
                static_cast<int64_t>(content.size()));
    }
    REQUIRE(vfs_close(file) == kOk);
}

} // namespace

TEST_CASE("vfs namei path resolution and dot-dot clamping", "[vfs][namei]") {
    vfs_reset();
    MockFsInstance root_fs = init_mock_fs("mock-root");
    REQUIRE(vfs_mount_root(root_fs.sb) == kOk);

    REQUIRE(vfs_mkdir("/dir", 0755U) == kOk);
    REQUIRE(vfs_mkdir("/dir/subdir", 0755U) == kOk);
    REQUIRE(vfs_mkdir("/other", 0755U) == kOk);
    seed_file(root_fs, "/dir/subdir/hello.txt", "hello");

    SECTION("resolves root, nested paths, redundant slashes, and dot components") {
        Dentry* dentry = nullptr;
        REQUIRE(vfs_namei("/", nullptr, &dentry) == kOk);
        REQUIRE(dentry == root_fs.sb->get_root());
        dentry->release();

        REQUIRE(vfs_namei("///dir////subdir///hello.txt", nullptr, &dentry) == kOk);
        REQUIRE(dentry != nullptr);
        REQUIRE(dentry->name_view() == "hello.txt");
        REQUIRE(dentry->get_inode()->get_size() == 5);
        dentry->release();

        REQUIRE(vfs_namei("/dir/./subdir/../subdir/./hello.txt", nullptr, &dentry) == kOk);
        REQUIRE(dentry != nullptr);
        REQUIRE(dentry->name_view() == "hello.txt");
        dentry->release();

        REQUIRE(vfs_namei("/dir/subdir/", nullptr, &dentry) == kOk);
        REQUIRE(dentry->name_view() == "subdir");
        dentry->release();

        REQUIRE(vfs_namei("/dir/subdir/hello.txt/", nullptr, &dentry) == kEnotdir);
    }

    SECTION("clamps dot-dot at global root and supports relative cwd") {
        Dentry* dentry = nullptr;
        REQUIRE(vfs_namei("/..", nullptr, &dentry) == kOk);
        REQUIRE(dentry == root_fs.sb->get_root());
        dentry->release();

        REQUIRE(vfs_namei("/../../../dir/../../..", nullptr, &dentry) == kOk);
        REQUIRE(dentry == root_fs.sb->get_root());
        dentry->release();

        Dentry* dir_cwd = nullptr;
        REQUIRE(vfs_namei("/dir", nullptr, &dir_cwd) == kOk);

        REQUIRE(vfs_namei("subdir/hello.txt", dir_cwd, &dentry) == kOk);
        REQUIRE(dentry->name_view() == "hello.txt");
        dentry->release();

        REQUIRE(vfs_namei("../other", dir_cwd, &dentry) == kOk);
        REQUIRE(dentry->name_view() == "other");
        dentry->release();

        dir_cwd->release();
    }

    SECTION("returns expected POSIX errors for invalid paths") {
        Dentry* dentry = nullptr;
        REQUIRE(vfs_namei("", nullptr, &dentry) == kEnoent);
        REQUIRE(vfs_namei("/does_not_exist", nullptr, &dentry) == kEnoent);
        REQUIRE(vfs_namei("/dir/subdir/hello.txt/child", nullptr, &dentry) == kEnotdir);

        Kernel::FS::StringView long_name =
            "/012345678901234567890123456789012345678901234567890123456789012345";
        REQUIRE(vfs_namei(long_name, nullptr, &dentry) == kEnametoolong);
    }
}

TEST_CASE("vfs mount table traversal and mount boundary crossing", "[vfs][mount]") {
    vfs_reset();
    MockFsInstance root_fs = init_mock_fs("mock-root");
    REQUIRE(vfs_mount_root(root_fs.sb) == kOk);

    REQUIRE(vfs_mkdir("/mnt", 0755U) == kOk);
    REQUIRE(vfs_mkdir("/mnt/sub", 0755U) == kOk);
    seed_file(root_fs, "/mnt/sub/underlying.txt", "hidden");

    MockFsInstance sub_fs = init_mock_fs("mock-sub");
    REQUIRE(vfs_mount("/mnt/sub", sub_fs.sb) == kOk);

    MockFsInstance extra_fs = init_mock_fs("mock-extra");
    REQUIRE(vfs_mount("/mnt/sub", extra_fs.sb) == kEbusy);
    if (extra_fs.sb->get_root() != nullptr) {
        extra_fs.sb->get_root()->release();
    }
    extra_fs.sb->reset();

    REQUIRE(vfs_mkdir("/mnt/sub/nested", 0755U) == kOk);
    seed_file(sub_fs, "/mnt/sub/nested/subfile.txt", "mounted-data");

    SECTION("descends into mounted superblock and hides underlying entries") {
        Dentry* dentry = nullptr;
        REQUIRE(vfs_namei("/mnt/sub", nullptr, &dentry) == kOk);
        REQUIRE(dentry == sub_fs.sb->get_root());
        dentry->release();

        REQUIRE(vfs_namei("/mnt/sub/nested/subfile.txt", nullptr, &dentry) == kOk);
        REQUIRE(dentry->get_sb() == sub_fs.sb);
        REQUIRE(dentry->name_view() == "subfile.txt");
        dentry->release();

        REQUIRE(vfs_namei("/mnt/sub/underlying.txt", nullptr, &dentry) == kEnoent);
    }

    SECTION("ascends across mount boundary via dot-dot") {
        Dentry* dentry = nullptr;
        REQUIRE(vfs_namei("/mnt/sub/..", nullptr, &dentry) == kOk);
        REQUIRE(dentry->name_view() == "mnt");
        REQUIRE(dentry->get_sb() == root_fs.sb);
        dentry->release();

        REQUIRE(vfs_namei("/mnt/sub/nested/../..", nullptr, &dentry) == kOk);
        REQUIRE(dentry->name_view() == "mnt");
        REQUIRE(dentry->get_sb() == root_fs.sb);
        dentry->release();

        REQUIRE(vfs_namei("/mnt/sub/../../..", nullptr, &dentry) == kOk);
        REQUIRE(dentry == root_fs.sb->get_root());
        dentry->release();
    }

    SECTION("enforces busy check on umount and restores underlying tree after umount") {
        Dentry* held = nullptr;
        REQUIRE(vfs_namei("/mnt/sub", nullptr, &held) == kOk);
        REQUIRE(vfs_umount("/mnt/sub") == kEbusy);
        held->release();

        REQUIRE(vfs_umount("/mnt/sub") == kOk);

        Dentry* underlying = nullptr;
        REQUIRE(vfs_namei("/mnt/sub/underlying.txt", nullptr, &underlying) == kOk);
        REQUIRE(underlying->get_sb() == root_fs.sb);
        underlying->release();
    }
}

TEST_CASE("vfs file I/O, directory mutations, and refcount lifecycle", "[vfs][ops]") {
    vfs_reset();
    MockFsInstance root_fs = init_mock_fs("mock-root");
    REQUIRE(vfs_mount_root(root_fs.sb) == kOk);

    const VfsStats baseline = vfs_stats();

    File* file = nullptr;
    File* dup = nullptr;
    REQUIRE(vfs_open("/notes.txt", kOpenCreate | kOpenExcl | kOpenReadWrite, 0644U, &file) == kOk);
    REQUIRE(vfs_open("/notes.txt", kOpenCreate | kOpenExcl | kOpenReadOnly, 0644U, &dup) ==
            kEexist);
    REQUIRE(dup == nullptr);

    const char* msg = "abcdef";
    REQUIRE(vfs_write(file, msg, 6) == 6);
    REQUIRE(vfs_lseek(file, 2, SeekWhence::kSet) == 2);

    char buf[8]{};
    REQUIRE(vfs_read(file, buf, 4) == 4);
    REQUIRE(Kernel::FS::StringView(buf, 4) == "cdef");
    REQUIRE(vfs_close(file) == kOk);

    REQUIRE(vfs_open("/notes.txt", kOpenWriteOnly | kOpenTrunc, 0644U, &file) == kOk);
    REQUIRE(file->get_inode()->get_size() == 0);
    REQUIRE(vfs_write(file, "xy", 2) == 2);
    REQUIRE(vfs_close(file) == kOk);

    REQUIRE(vfs_mkdir("/work", 0755U) == kOk);
    REQUIRE(vfs_rename("/notes.txt", "/work/moved.txt") == kOk);

    Dentry* missing = nullptr;
    REQUIRE(vfs_namei("/notes.txt", nullptr, &missing) == kEnoent);

    File* dir_file = nullptr;
    REQUIRE(vfs_open("/work", kOpenReadOnly | kOpenDirectory, 0, &dir_file) == kOk);
    DirEntryInfo entry{};
    REQUIRE(vfs_readdir(dir_file, &entry) == kOk);
    REQUIRE(Kernel::FS::StringView(entry.name) == "moved.txt");
    REQUIRE(vfs_readdir(dir_file, &entry) == kEnoent);
    REQUIRE(vfs_close(dir_file) == kOk);

    REQUIRE(vfs_rmdir("/work") == kEnotempty);

    File* open_unlinked = nullptr;
    REQUIRE(vfs_open("/work/moved.txt", kOpenReadOnly, 0, &open_unlinked) == kOk);
    REQUIRE(vfs_unlink("/work/moved.txt") == kOk);

    char still_readable[4]{};
    REQUIRE(vfs_read(open_unlinked, still_readable, 2) == 2);
    REQUIRE(Kernel::FS::StringView(still_readable, 2) == "xy");
    REQUIRE(vfs_close(open_unlinked) == kOk);

    REQUIRE(vfs_rmdir("/work") == kOk);

    const VfsStats final_stats = vfs_stats();
    REQUIRE(final_stats.active_inodes == baseline.active_inodes);
    REQUIRE(final_stats.active_dentries == baseline.active_dentries);
    REQUIRE(final_stats.active_files == baseline.active_files);
}

TEST_CASE("vfs read-only superblock, cross-mount rename, and lazy lookup", "[vfs][errors]") {
    vfs_reset();
    MockFsInstance root_fs = init_mock_fs("mock-root");
    REQUIRE(vfs_mount_root(root_fs.sb) == kOk);

    REQUIRE(vfs_mkdir("/ro", 0755U) == kOk);
    REQUIRE(vfs_mkdir("/rw", 0755U) == kOk);
    seed_file(root_fs, "/rw/item.txt", "data");

    MockFsInstance ro_fs = init_mock_fs("mock-ro", kSbReadOnly);
    REQUIRE(vfs_mount("/ro", ro_fs.sb) == kOk);

    SECTION("enforces EROFS across all mutating VFS entry points") {
        File* file = nullptr;
        REQUIRE(vfs_open("/ro/new.txt", kOpenCreate | kOpenWriteOnly, 0644U, &file) == kErofs);
        REQUIRE(vfs_mkdir("/ro/sub", 0755U) == kErofs);
        REQUIRE(vfs_unlink("/ro/anything") == kErofs);
        REQUIRE(vfs_rmdir("/ro/anything") == kErofs);
        REQUIRE(vfs_rename("/ro/a", "/ro/b") == kErofs);
    }

    SECTION("enforces EXDEV, EISDIR, ENOTDIR, and ancestor loop guards") {
        REQUIRE(vfs_rename("/rw/item.txt", "/ro/item.txt") == kExdev);

        REQUIRE(vfs_mkdir("/rw/parent", 0755U) == kOk);
        REQUIRE(vfs_mkdir("/rw/parent/child", 0755U) == kOk);
        REQUIRE(vfs_rename("/rw/parent", "/rw/parent/child/moved") == kEinval);

        File* file = nullptr;
        REQUIRE(vfs_open("/rw/item.txt", kOpenReadOnly | kOpenDirectory, 0, &file) == kEnotdir);
        REQUIRE(vfs_open("/rw/parent", kOpenWriteOnly, 0, &file) == kEisdir);
        REQUIRE(vfs_open("/rw/parent", kOpenReadOnly | kOpenTrunc, 0, &file) == kEisdir);
        REQUIRE(vfs_unlink("/rw/parent") == kEisdir);
        REQUIRE(vfs_rmdir("/rw/item.txt") == kEnotdir);

        REQUIRE(vfs_open("/rw/item.txt", kOpenReadWrite | kOpenAppend, 0, &file) == kOk);
        REQUIRE(vfs_write(file, "!", 1) == 1);
        REQUIRE(vfs_lseek(file, -2, SeekWhence::kEnd) == 3);
        REQUIRE(vfs_lseek(file, 1, SeekWhence::kCur) == 4);
        REQUIRE(vfs_lseek(file, -10, SeekWhence::kSet) == kEinval);
        char chr = 0;
        REQUIRE(vfs_read(file, &chr, 1) == 1);
        REQUIRE(chr == '!');
        REQUIRE(vfs_close(file) == kOk);
    }

    SECTION("supports lazy filesystem lookup callback on cache miss") {
        static bool lookup_invoked = false;
        static InodeOperations lazy_ops = kMockInodeOps;
        lazy_ops.lookup = [](Inode* dir, Dentry* target) noexcept -> int32_t {
            lookup_invoked = true;
            if (target->name_view() != "lazy_file") {
                return kEnoent;
            }
            Inode* ino = vfs_alloc_inode(dir->get_sb());
            if (ino == nullptr) {
                return kEnomem;
            }
            ino->set_ino(999);
            ino->set_mode(kModeReg | 0444U);
            ino->set_f_ops(&kMockFileOps);
            target->bind_inode(ino);
            return kOk;
        };

        root_fs.sb->get_root()->get_inode()->set_i_ops(&lazy_ops);

        Dentry* resolved = nullptr;
        REQUIRE(vfs_namei("/lazy_file", nullptr, &resolved) == kOk);
        REQUIRE(lookup_invoked);
        REQUIRE(resolved != nullptr);
        REQUIRE(resolved->get_inode()->get_ino() == 999);
        resolved->release();

        lookup_invoked = false;
        REQUIRE(vfs_namei("/lazy_file", nullptr, &resolved) == kOk);
        REQUIRE(!lookup_invoked);
        resolved->release();
    }
}
