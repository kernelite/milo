#include "fs/devfs/devfs.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "ipc/Pipe.hpp"
#include "syscalls/SyscallInternal.hpp"

#include <new>

namespace Kernel::Syscalls {

// ==========================================================================
// VFS, Directory, Pipe & Descriptor Syscalls
// ==========================================================================

int64_t sys_getcwd(uintptr_t user_buf, uint64_t size) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (size == 0U) {
        return ERR_EINVAL;
    }
    const size_t len = cstr_len(proc->cwd()) + 1U;
    if (size < len) {
        return ERR_ERANGE;
    }
    const int64_t res = Kernel::MM::copy_to_user(proc->address_space(), user_buf, proc->cwd(), len);
    if (res < 0) {
        return ERR_EFAULT;
    }
    return static_cast<int64_t>(len);
}

int64_t sys_chdir(uintptr_t user_path) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space().root_paddr(), user_path,
                                               raw_path, sizeof(raw_path), len);
    if (copy_res < 0) {
        return copy_res;
    }

    Kernel::FS::Dentry* cwd_dentry = nullptr;
    if (raw_path[0] != '/') {
        (void)Kernel::FS::vfs_namei(Kernel::FS::StringView(proc->cwd()), nullptr, &cwd_dentry);
    }

    Kernel::FS::Dentry* target = nullptr;
    const int32_t vfs_res =
        Kernel::FS::vfs_namei(Kernel::FS::StringView(raw_path, len), cwd_dentry, &target);
    if (cwd_dentry != nullptr) {
        Kernel::FS::dentry_put(cwd_dentry);
    }
    if (vfs_res != Kernel::FS::kOk) {
        return vfs_res;
    }
    if (target->inode == nullptr || !target->inode->is_dir()) {
        Kernel::FS::dentry_put(target);
        return ERR_ENOTDIR;
    }

    build_dentry_path(target, proc->mutable_cwd(), Kernel::Process::MAX_PATH_LEN);
    Kernel::FS::dentry_put(target);
    return 0;
}

int64_t sys_openat(int32_t dirfd, uintptr_t user_pathname, uint64_t flags, uint64_t mode) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space().root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    Kernel::FS::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const uint64_t accmode = flags & O_ACCMODE;
    uint32_t vfs_flags = 0U;
    if (accmode == O_RDONLY) {
        vfs_flags = Kernel::FS::kOpenReadOnly;
    } else if (accmode == O_WRONLY) {
        vfs_flags = Kernel::FS::kOpenWriteOnly;
    } else if (accmode == O_RDWR) {
        vfs_flags = Kernel::FS::kOpenReadWrite;
    } else {
        if (base_dentry != nullptr) {
            Kernel::FS::dentry_put(base_dentry);
        }
        return ERR_EINVAL;
    }

    if ((flags & O_CREAT) != 0ULL) {
        vfs_flags |= Kernel::FS::kOpenCreate;
    }
    if ((flags & O_EXCL) != 0ULL) {
        vfs_flags |= Kernel::FS::kOpenExcl;
    }
    if ((flags & O_TRUNC) != 0ULL) {
        vfs_flags |= Kernel::FS::kOpenTrunc;
    }
    if ((flags & O_APPEND) != 0ULL) {
        vfs_flags |= Kernel::FS::kOpenAppend;
    }
    if ((flags & O_DIRECTORY) != 0ULL) {
        vfs_flags |= Kernel::FS::kOpenDirectory;
    }
    if ((flags & O_NONBLOCK) != 0ULL) {
        vfs_flags |= Kernel::FS::kOpenNonBlock;
    }

    const int slot = proc->fds().allocate(proc->rlim_nofile().rlim_cur);
    if (slot < 0) {
        if (base_dentry != nullptr) {
            Kernel::FS::dentry_put(base_dentry);
        }
        return slot;
    }

    const uint32_t effective_mode = static_cast<uint32_t>(mode) & ~proc->umask();
    Kernel::FS::File* file = nullptr;
    const int32_t open_rc = Kernel::FS::vfs_open_at(
        base_dentry, Kernel::FS::StringView(raw_path, path_len), vfs_flags, effective_mode, &file);
    if (base_dentry != nullptr) {
        Kernel::FS::dentry_put(base_dentry);
    }
    if (open_rc != Kernel::FS::kOk) {
        return open_rc;
    }

    const uint32_t fd_flags = ((flags & O_CLOEXEC) != 0ULL) ? Kernel::Process::FD_FLAG_CLOEXEC : 0U;
    (void)proc->fds().install(static_cast<size_t>(slot), file, fd_flags);
    return slot;
}

int64_t sys_mkdirat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space().root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    Kernel::FS::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const uint32_t effective_mode = static_cast<uint32_t>(mode) & ~proc->umask();
    const int32_t vfs_res = Kernel::FS::vfs_mkdir_at(
        base_dentry, Kernel::FS::StringView(raw_path, path_len), effective_mode);
    if (base_dentry != nullptr) {
        Kernel::FS::dentry_put(base_dentry);
    }
    return vfs_res;
}

int64_t sys_unlinkat(int32_t dirfd, uintptr_t user_pathname, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~AT_REMOVEDIR) != 0ULL) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space().root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    Kernel::FS::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const Kernel::FS::StringView path_view(raw_path, path_len);
    const int32_t vfs_res = ((flags & AT_REMOVEDIR) != 0ULL)
                                ? Kernel::FS::vfs_rmdir_at(base_dentry, path_view)
                                : Kernel::FS::vfs_unlink_at(base_dentry, path_view);
    if (base_dentry != nullptr) {
        Kernel::FS::dentry_put(base_dentry);
    }
    return vfs_res;
}

int64_t sys_renameat2(int32_t olddirfd, uintptr_t user_oldpath, int32_t newdirfd,
                      uintptr_t user_newpath, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~RENAME_NOREPLACE) != 0ULL) {
        return ERR_EINVAL;
    }

    char old_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t old_len = 0U;
    const int64_t old_copy = copy_user_cstring(proc->address_space().root_paddr(), user_oldpath,
                                               old_path, sizeof(old_path), old_len);
    if (old_copy < 0) {
        return old_copy;
    }

    char new_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t new_len = 0U;
    const int64_t new_copy = copy_user_cstring(proc->address_space().root_paddr(), user_newpath,
                                               new_path, sizeof(new_path), new_len);
    if (new_copy < 0) {
        return new_copy;
    }

    Kernel::FS::Dentry* old_base = nullptr;
    if (old_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, olddirfd, old_base);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    Kernel::FS::Dentry* new_base = nullptr;
    if (new_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, newdirfd, new_base);
        if (dir_res < 0) {
            if (old_base != nullptr) {
                Kernel::FS::dentry_put(old_base);
            }
            return dir_res;
        }
    }

    if ((flags & RENAME_NOREPLACE) != 0ULL) {
        Kernel::FS::Dentry* existing = nullptr;
        if (Kernel::FS::vfs_namei(Kernel::FS::StringView(new_path, new_len), new_base, &existing,
                                  false) == Kernel::FS::kOk) {
            Kernel::FS::dentry_put(existing);
            if (new_base != nullptr) {
                Kernel::FS::dentry_put(new_base);
            }
            if (old_base != nullptr) {
                Kernel::FS::dentry_put(old_base);
            }
            return ERR_EEXIST;
        }
    }

    const int32_t vfs_res =
        Kernel::FS::vfs_rename_at(old_base, Kernel::FS::StringView(old_path, old_len), new_base,
                                  Kernel::FS::StringView(new_path, new_len));
    if (new_base != nullptr) {
        Kernel::FS::dentry_put(new_base);
    }
    if (old_base != nullptr) {
        Kernel::FS::dentry_put(old_base);
    }
    return vfs_res;
}

int64_t sys_renameat(int32_t olddirfd, uintptr_t user_oldpath, int32_t newdirfd,
                     uintptr_t user_newpath) noexcept {
    return sys_renameat2(olddirfd, user_oldpath, newdirfd, user_newpath, 0ULL);
}

int64_t sys_ftruncate(uint64_t raw_fd, int64_t length) noexcept {
    if (length < 0) {
        return ERR_EINVAL;
    }
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EBADF;
    }
    const auto* const desc = proc->fds().at(static_cast<size_t>(raw_fd));
    if (desc == nullptr || !desc->is_open() || !desc->is_writable() ||
        desc->file()->inode == nullptr) {
        return ERR_EBADF;
    }
    Kernel::FS::Inode* inode = desc->file()->inode;
    if (inode->sb != nullptr && inode->sb->is_read_only()) {
        return ERR_EROFS;
    }
    if (inode->i_ops == nullptr || inode->i_ops->truncate == nullptr) {
        return ERR_EINVAL;
    }
    return inode->i_ops->truncate(inode, static_cast<uint64_t>(length));
}

int64_t sys_truncate(uintptr_t user_pathname, int64_t length) noexcept {
    if (length < 0) {
        return ERR_EINVAL;
    }
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space().root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    Kernel::FS::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, AT_FDCWD, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    Kernel::FS::Dentry* target = nullptr;
    const int32_t vfs_res = Kernel::FS::vfs_namei(Kernel::FS::StringView(raw_path, path_len),
                                                  base_dentry, &target, true);
    if (base_dentry != nullptr) {
        Kernel::FS::dentry_put(base_dentry);
    }
    if (vfs_res != Kernel::FS::kOk || target == nullptr || target->inode == nullptr) {
        if (target != nullptr) {
            Kernel::FS::dentry_put(target);
        }
        return (vfs_res != Kernel::FS::kOk) ? vfs_res : ERR_ENOENT;
    }

    Kernel::FS::Inode* inode = target->inode;
    int64_t res = 0;
    if (inode->sb != nullptr && inode->sb->is_read_only()) {
        res = ERR_EROFS;
    } else if (inode->i_ops == nullptr || inode->i_ops->truncate == nullptr) {
        res = ERR_EINVAL;
    } else {
        res = inode->i_ops->truncate(inode, static_cast<uint64_t>(length));
    }
    Kernel::FS::dentry_put(target);
    return res;
}

int64_t sys_utimensat(int32_t dirfd, uintptr_t user_pathname, uintptr_t user_times,
                      uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0ULL) {
        return ERR_EINVAL;
    }

    linux_timespec times[2]{
        {.tv_sec = 0, .tv_nsec = UTIME_NOW},
        {.tv_sec = 0, .tv_nsec = UTIME_NOW},
    };
    Kernel::Time::Duration parsed_times[2]{};
    if (user_times != 0U) {
        if (Kernel::MM::copy_from_user(proc->address_space(), times, user_times, sizeof(times)) <
            0) {
            return ERR_EFAULT;
        }
        for (size_t idx = 0U; idx < 2U; ++idx) {
            if (times[idx].tv_nsec != UTIME_NOW && times[idx].tv_nsec != UTIME_OMIT &&
                !Kernel::Time::Duration::from_linux_timespec(times[idx], parsed_times[idx])) {
                return ERR_EINVAL;
            }
        }
    }

    Kernel::FS::Inode* inode = nullptr;
    Kernel::FS::Dentry* target = nullptr;

    if (user_pathname == 0U) {
        Kernel::FS::File* file = get_proc_file(proc, static_cast<uint64_t>(dirfd));
        if (file == nullptr || file->inode == nullptr) {
            return ERR_EBADF;
        }
        inode = file->inode;
    } else {
        char raw_path[Kernel::Process::MAX_PATH_LEN]{};
        size_t path_len = 0U;
        const int64_t copy_res =
            copy_user_cstring(proc->address_space().root_paddr(), user_pathname, raw_path,
                              sizeof(raw_path), path_len);
        if (copy_res < 0) {
            return copy_res;
        }

        if (path_len == 0U && (flags & AT_EMPTY_PATH) != 0ULL) {
            Kernel::FS::File* file = get_proc_file(proc, static_cast<uint64_t>(dirfd));
            if (file == nullptr || file->inode == nullptr) {
                return ERR_EBADF;
            }
            inode = file->inode;
        } else {
            Kernel::FS::Dentry* base_dentry = nullptr;
            if (raw_path[0] != '/') {
                const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
                if (dir_res < 0) {
                    return dir_res;
                }
            }
            const bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0ULL;
            const int32_t vfs_res = Kernel::FS::vfs_namei(
                Kernel::FS::StringView(raw_path, path_len), base_dentry, &target, follow);
            if (base_dentry != nullptr) {
                Kernel::FS::dentry_put(base_dentry);
            }
            if (vfs_res != Kernel::FS::kOk || target == nullptr || target->inode == nullptr) {
                if (target != nullptr) {
                    Kernel::FS::dentry_put(target);
                }
                return (vfs_res != Kernel::FS::kOk) ? vfs_res : ERR_ENOENT;
            }
            inode = target->inode;
        }
    }

    int64_t res = 0;
    if (inode->sb != nullptr && inode->sb->is_read_only()) {
        res = ERR_EROFS;
    } else {
        const uint64_t now_ns = monotonic_now_ns();
        if (times[0].tv_nsec == UTIME_NOW) {
            inode->atime_ns = now_ns;
        } else if (times[0].tv_nsec != UTIME_OMIT) {
            inode->atime_ns = parsed_times[0].nanoseconds();
        }
        if (times[1].tv_nsec == UTIME_NOW) {
            inode->mtime_ns = now_ns;
        } else if (times[1].tv_nsec != UTIME_OMIT) {
            inode->mtime_ns = parsed_times[1].nanoseconds();
        }
        inode->ctime_ns = now_ns;
    }

    if (target != nullptr) {
        Kernel::FS::dentry_put(target);
    }
    return res;
}

int64_t sys_fsync(uint64_t raw_fd, int32_t datasync) noexcept {
    auto* const proc = Kernel::Process::current_process();
    Kernel::FS::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr) {
        return ERR_EBADF;
    }
    if (file->f_ops != nullptr && file->f_ops->fsync != nullptr) {
        return file->f_ops->fsync(file, datasync);
    }
    return 0;
}

int64_t sys_fchmod(uint64_t raw_fd, uint64_t mode) noexcept {
    auto* const proc = Kernel::Process::current_process();
    Kernel::FS::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr || file->inode == nullptr) {
        return ERR_EBADF;
    }
    if (file->inode->sb != nullptr && file->inode->sb->is_read_only()) {
        return ERR_EROFS;
    }
    file->inode->mode =
        (file->inode->mode & Kernel::FS::kModeTypeMask) | (static_cast<uint32_t>(mode) & 07777U);
    file->inode->ctime_ns = monotonic_now_ns();
    return 0;
}

int64_t sys_fchmodat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                     uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0ULL) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space().root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    Kernel::FS::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0ULL;
    Kernel::FS::Dentry* target = nullptr;
    const int32_t vfs_res = Kernel::FS::vfs_namei(Kernel::FS::StringView(raw_path, path_len),
                                                  base_dentry, &target, follow);
    if (base_dentry != nullptr) {
        Kernel::FS::dentry_put(base_dentry);
    }
    if (vfs_res != Kernel::FS::kOk || target == nullptr || target->inode == nullptr) {
        if (target != nullptr) {
            Kernel::FS::dentry_put(target);
        }
        return (vfs_res != Kernel::FS::kOk) ? vfs_res : ERR_ENOENT;
    }

    int64_t res = 0;
    if (target->inode->sb != nullptr && target->inode->sb->is_read_only()) {
        res = ERR_EROFS;
    } else {
        target->inode->mode = (target->inode->mode & Kernel::FS::kModeTypeMask) |
                              (static_cast<uint32_t>(mode) & 07777U);
        target->inode->ctime_ns = monotonic_now_ns();
    }
    Kernel::FS::dentry_put(target);
    return res;
}

int64_t sys_fchown(uint64_t raw_fd, uint32_t owner, uint32_t group) noexcept {
    auto* const proc = Kernel::Process::current_process();
    Kernel::FS::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr || file->inode == nullptr) {
        return ERR_EBADF;
    }
    if (file->inode->sb != nullptr && file->inode->sb->is_read_only()) {
        return ERR_EROFS;
    }
    if (owner != static_cast<uint32_t>(-1)) {
        file->inode->uid = owner;
    }
    if (group != static_cast<uint32_t>(-1)) {
        file->inode->gid = group;
    }
    file->inode->ctime_ns = monotonic_now_ns();
    return 0;
}

int64_t sys_fchownat(int32_t dirfd, uintptr_t user_pathname, uint32_t owner, uint32_t group,
                     uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0ULL) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space().root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    if (path_len == 0U && (flags & AT_EMPTY_PATH) != 0ULL) {
        return sys_fchown(static_cast<uint64_t>(dirfd), owner, group);
    }

    Kernel::FS::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0ULL;
    Kernel::FS::Dentry* target = nullptr;
    const int32_t vfs_res = Kernel::FS::vfs_namei(Kernel::FS::StringView(raw_path, path_len),
                                                  base_dentry, &target, follow);
    if (base_dentry != nullptr) {
        Kernel::FS::dentry_put(base_dentry);
    }
    if (vfs_res != Kernel::FS::kOk || target == nullptr || target->inode == nullptr) {
        if (target != nullptr) {
            Kernel::FS::dentry_put(target);
        }
        return (vfs_res != Kernel::FS::kOk) ? vfs_res : ERR_ENOENT;
    }

    int64_t res = 0;
    if (target->inode->sb != nullptr && target->inode->sb->is_read_only()) {
        res = ERR_EROFS;
    } else {
        if (owner != static_cast<uint32_t>(-1)) {
            target->inode->uid = owner;
        }
        if (group != static_cast<uint32_t>(-1)) {
            target->inode->gid = group;
        }
        target->inode->ctime_ns = monotonic_now_ns();
    }
    Kernel::FS::dentry_put(target);
    return res;
}

int64_t sys_close(uint64_t raw_fd) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EBADF;
    }
    const auto fd_idx = static_cast<size_t>(raw_fd);
    const auto* const desc = proc->fds().at(fd_idx);
    if (desc == nullptr || !desc->is_open()) {
        return ERR_EBADF;
    }
    proc->fds().close(fd_idx);
    return 0;
}

int64_t sys_lseek(uint64_t raw_fd, int64_t offset, uint64_t whence) noexcept {
    auto* const proc = Kernel::Process::current_process();
    Kernel::FS::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr) {
        return ERR_EBADF;
    }
    if (whence > SEEK_END) {
        return ERR_EINVAL;
    }
    return Kernel::FS::vfs_lseek(file, offset,
                                 static_cast<Kernel::FS::SeekWhence>(static_cast<int32_t>(whence)));
}

int64_t sys_fstat(uint64_t raw_fd, uintptr_t user_statbuf) noexcept {
    auto* const proc = Kernel::Process::current_process();
    Kernel::FS::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr || file->inode == nullptr) {
        return ERR_EBADF;
    }
    const linux_stat stat = inode_to_linux_stat(*file->inode);
    return Kernel::MM::copy_to_user(proc->address_space(), user_statbuf, &stat, sizeof(stat));
}

int64_t sys_newfstatat(int32_t dirfd, uintptr_t user_pathname, uintptr_t user_statbuf,
                       uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space().root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    if (path_len == 0U && (flags & AT_EMPTY_PATH) != 0ULL) {
        Kernel::FS::File* file = get_proc_file(proc, static_cast<uint64_t>(dirfd));
        if (file == nullptr || file->inode == nullptr) {
            return ERR_EBADF;
        }
        const linux_stat stat = inode_to_linux_stat(*file->inode);
        return Kernel::MM::copy_to_user(proc->address_space(), user_statbuf, &stat, sizeof(stat));
    }

    Kernel::FS::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    const bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0ULL;
    Kernel::FS::Dentry* target = nullptr;
    const int32_t vfs_res = Kernel::FS::vfs_namei(Kernel::FS::StringView(raw_path, path_len),
                                                  base_dentry, &target, follow);
    if (base_dentry != nullptr) {
        Kernel::FS::dentry_put(base_dentry);
    }
    if (vfs_res != Kernel::FS::kOk || target->inode == nullptr) {
        if (target != nullptr) {
            Kernel::FS::dentry_put(target);
        }
        return (vfs_res != Kernel::FS::kOk) ? vfs_res : ERR_ENOENT;
    }

    const linux_stat stat = inode_to_linux_stat(*target->inode);
    Kernel::FS::dentry_put(target);
    return Kernel::MM::copy_to_user(proc->address_space(), user_statbuf, &stat, sizeof(stat));
}

int64_t sys_faccessat2(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                       uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EACCESS | AT_EMPTY_PATH)) != 0ULL ||
        (mode & ~7ULL) != 0ULL) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space().root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    Kernel::FS::Inode* inode = nullptr;
    Kernel::FS::Dentry* target = nullptr;

    if (path_len == 0U && (flags & AT_EMPTY_PATH) != 0ULL) {
        Kernel::FS::File* file = get_proc_file(proc, static_cast<uint64_t>(dirfd));
        if (file == nullptr || file->inode == nullptr) {
            return ERR_EBADF;
        }
        inode = file->inode;
    } else {
        Kernel::FS::Dentry* base_dentry = nullptr;
        if (raw_path[0] != '/') {
            const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
            if (dir_res < 0) {
                return dir_res;
            }
        }
        const bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0ULL;
        const int32_t vfs_res = Kernel::FS::vfs_namei(Kernel::FS::StringView(raw_path, path_len),
                                                      base_dentry, &target, follow);
        if (base_dentry != nullptr) {
            Kernel::FS::dentry_put(base_dentry);
        }
        if (vfs_res != Kernel::FS::kOk || target->inode == nullptr) {
            if (target != nullptr) {
                Kernel::FS::dentry_put(target);
            }
            return (vfs_res != Kernel::FS::kOk) ? vfs_res : ERR_ENOENT;
        }
        inode = target->inode;
    }

    int64_t res = 0;
    if ((mode & 2ULL) != 0ULL && inode->sb != nullptr && inode->sb->is_read_only()) {
        res = ERR_EROFS;
    } else if ((mode & 1ULL) != 0ULL && !inode->is_dir() && (inode->mode & 0111U) == 0U) {
        res = ERR_EACCES;
    }

    if (target != nullptr) {
        Kernel::FS::dentry_put(target);
    }
    return res;
}

int64_t sys_faccessat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                      uint64_t flags) noexcept {
    return sys_faccessat2(dirfd, user_pathname, mode, flags);
}

int64_t sys_readlinkat(int32_t dirfd, uintptr_t user_pathname, uintptr_t user_buf,
                       uint64_t bufsiz) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || bufsiz == 0U) {
        return ERR_EINVAL;
    }

    char raw_path[Kernel::Process::MAX_PATH_LEN]{};
    size_t path_len = 0U;
    const int64_t copy_res = copy_user_cstring(proc->address_space().root_paddr(), user_pathname,
                                               raw_path, sizeof(raw_path), path_len);
    if (copy_res < 0) {
        return copy_res;
    }

    if (cstr_eq(raw_path, "/proc/self/exe")) {
        const char* exe_target = (proc->exe_path()[0] != '\0') ? proc->exe_path() : "/bin/bash";
        const size_t target_len = cstr_len(exe_target);
        const size_t copy_len = (bufsiz < target_len) ? static_cast<size_t>(bufsiz) : target_len;
        if (Kernel::MM::copy_to_user(proc->address_space(), user_buf, exe_target, copy_len) < 0) {
            return ERR_EFAULT;
        }
        return static_cast<int64_t>(copy_len);
    }

    Kernel::FS::Dentry* base_dentry = nullptr;
    if (raw_path[0] != '/') {
        const int64_t dir_res = resolve_dirfd_dentry(*proc, dirfd, base_dentry);
        if (dir_res < 0) {
            return dir_res;
        }
    }

    char link_buf[Kernel::Process::MAX_PATH_LEN]{};
    size_t out_len = 0U;
    const int32_t vfs_res =
        Kernel::FS::vfs_readlink(Kernel::FS::StringView(raw_path, path_len), base_dentry, link_buf,
                                 sizeof(link_buf), &out_len);
    if (base_dentry != nullptr) {
        Kernel::FS::dentry_put(base_dentry);
    }
    if (vfs_res != Kernel::FS::kOk) {
        return vfs_res;
    }

    const size_t copy_len = (bufsiz < out_len) ? static_cast<size_t>(bufsiz) : out_len;
    if (copy_len > 0U &&
        Kernel::MM::copy_to_user(proc->address_space(), user_buf, link_buf, copy_len) < 0) {
        return ERR_EFAULT;
    }
    return static_cast<int64_t>(copy_len);
}

int64_t sys_getdents64(uint64_t raw_fd, uintptr_t user_dirp, uint64_t count) noexcept {
    auto* const proc = Kernel::Process::current_process();
    Kernel::FS::File* file = get_proc_file(proc, raw_fd);
    if (file == nullptr || file->inode == nullptr) {
        return ERR_EBADF;
    }
    if (!file->inode->is_dir()) {
        return ERR_ENOTDIR;
    }

    const uintptr_t root_paddr = proc->address_space().root_paddr();
    if (!validate_user_write_range(root_paddr, user_dirp, static_cast<size_t>(count))) {
        return ERR_EFAULT;
    }

    size_t bytes_written = 0U;
    auto push_dirent = [&](uint64_t ino, uint8_t d_type, const char* name, size_t name_len,
                           uint64_t next_off) -> bool {
        const size_t raw_reclen = sizeof(linux_dirent64_hdr) + name_len + 1U;
        const size_t aligned_reclen = (raw_reclen + 7U) & ~7U;
        if (bytes_written + aligned_reclen > count || aligned_reclen > 128U) {
            return false;
        }
        uint8_t record[128]{};
        auto* const hdr = reinterpret_cast<linux_dirent64_hdr*>(record);
        hdr->d_ino = ino;
        hdr->d_off = static_cast<int64_t>(next_off);
        hdr->d_reclen = static_cast<uint16_t>(aligned_reclen);
        hdr->d_type = d_type;
        for (size_t idx = 0U; idx < name_len; ++idx) {
            record[sizeof(linux_dirent64_hdr) + idx] = static_cast<uint8_t>(name[idx]);
        }
        if (Kernel::MM::copy_to_user(root_paddr, user_dirp + bytes_written, record,
                                     aligned_reclen) < 0) {
            return false;
        }
        bytes_written += aligned_reclen;
        return true;
    };

    if (file->offset == 0U) {
        if (!push_dirent(file->inode->ino, DT_DIR, ".", 1U, 1U)) {
            return static_cast<int64_t>(bytes_written);
        }
        file->offset = 1U;
    }

    if (file->offset == 1U) {
        const uint64_t parent_ino = (file->dentry != nullptr && file->dentry->parent != nullptr &&
                                     file->dentry->parent->inode != nullptr)
                                        ? file->dentry->parent->inode->ino
                                        : file->inode->ino;
        if (!push_dirent(parent_ino, DT_DIR, "..", 2U, 2U)) {
            return static_cast<int64_t>(bytes_written);
        }
        file->offset = 2U;
    }

    while (bytes_written < count) {
        const uint64_t child_idx = file->offset - 2U;
        file->offset = child_idx;
        Kernel::FS::DirEntryInfo info{};
        const int32_t vfs_res = Kernel::FS::vfs_readdir(file, &info);
        file->offset = child_idx + 2U;
        if (vfs_res != Kernel::FS::kOk) {
            break;
        }

        uint8_t dtype = DT_REG;
        if (info.type == Kernel::FS::kModeDir) {
            dtype = DT_DIR;
        } else if (info.type == Kernel::FS::kModeLnk) {
            dtype = DT_LNK;
        }

        const size_t name_len = cstr_len(info.name);
        if (!push_dirent(info.ino, dtype, info.name, name_len, child_idx + 3U)) {
            break;
        }
        file->offset = child_idx + 3U;
    }

    return static_cast<int64_t>(bytes_written);
}

int64_t sys_dup(uint64_t raw_oldfd) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EBADF;
    }
    const auto old_idx = static_cast<size_t>(raw_oldfd);
    const auto* const old_desc = proc->fds().at(old_idx);
    if (old_desc == nullptr || !old_desc->is_open()) {
        return ERR_EBADF;
    }

    const int new_idx = proc->fds().allocate(proc->rlim_nofile().rlim_cur, 0U);
    if (new_idx < 0) {
        return new_idx;
    }

    (void)proc->fds().dup_slot(static_cast<size_t>(new_idx), old_idx, true);
    return new_idx;
}

int64_t sys_dup3(uint64_t raw_oldfd, uint64_t raw_newfd, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EBADF;
    }
    if (raw_newfd >= proc->rlim_nofile().rlim_cur) {
        return ERR_EBADF;
    }
    if (raw_oldfd == raw_newfd) {
        return ERR_EINVAL;
    }
    if ((flags & ~O_CLOEXEC) != 0ULL) {
        return ERR_EINVAL;
    }

    const auto old_idx = static_cast<size_t>(raw_oldfd);
    const auto new_idx = static_cast<size_t>(raw_newfd);
    const auto* const old_desc = proc->fds().at(old_idx);
    if (old_desc == nullptr || !old_desc->is_open()) {
        return ERR_EBADF;
    }

    if (new_idx >= proc->fds().capacity()) {
        if (!proc->fds().ensure_capacity(new_idx + 1U, proc->rlim_nofile().rlim_cur)) {
            return ERR_ENOMEM;
        }
    }

    (void)proc->fds().dup_slot(new_idx, old_idx, true);
    if ((flags & O_CLOEXEC) != 0ULL) {
        if (auto* const dst_desc = proc->fds().at(new_idx); dst_desc != nullptr) {
            dst_desc->set_cloexec(true);
        }
    }
    return static_cast<int64_t>(new_idx);
}

int64_t sys_fcntl(uint64_t raw_fd, uint64_t cmd, uint64_t arg) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EBADF;
    }
    const auto fd_idx = static_cast<size_t>(raw_fd);
    auto* const desc = proc->fds().at(fd_idx);
    if (desc == nullptr || !desc->is_open()) {
        return ERR_EBADF;
    }

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if (arg >= proc->rlim_nofile().rlim_cur) {
            return ERR_EINVAL;
        }
        const int new_idx =
            proc->fds().allocate(proc->rlim_nofile().rlim_cur, static_cast<size_t>(arg));
        if (new_idx < 0) {
            return new_idx;
        }
        (void)proc->fds().dup_slot(static_cast<size_t>(new_idx), fd_idx, true);
        if (cmd == F_DUPFD_CLOEXEC) {
            if (auto* const dst_desc = proc->fds().at(static_cast<size_t>(new_idx));
                dst_desc != nullptr) {
                dst_desc->set_cloexec(true);
            }
        }
        return new_idx;
    }

    case F_GETFD:
        return desc->has_cloexec() ? static_cast<int64_t>(FD_CLOEXEC_BIT) : 0;

    case F_SETFD:
        desc->set_cloexec((arg & FD_CLOEXEC_BIT) != 0ULL);
        return 0;

    case F_GETFL: {
        uint64_t flags = O_RDONLY;
        if (desc->is_readable() && desc->is_writable()) {
            flags = O_RDWR;
        } else if (desc->is_writable()) {
            flags = O_WRONLY;
        }
        if ((desc->file()->flags & Kernel::FS::kOpenNonBlock) != 0U) {
            flags |= O_NONBLOCK;
        }
        if ((desc->file()->flags & Kernel::FS::kOpenAppend) != 0U) {
            flags |= O_APPEND;
        }
        return static_cast<int64_t>(flags);
    }

    case F_SETFL:
        if ((arg & O_NONBLOCK) != 0ULL) {
            desc->file()->flags |= Kernel::FS::kOpenNonBlock;
        } else {
            desc->file()->flags &= ~Kernel::FS::kOpenNonBlock;
        }
        if ((arg & O_APPEND) != 0ULL) {
            desc->file()->flags |= Kernel::FS::kOpenAppend;
        } else {
            desc->file()->flags &= ~Kernel::FS::kOpenAppend;
        }
        return 0;

    default:
        return ERR_EINVAL;
    }
}

int64_t sys_pipe2(uintptr_t user_pipefd, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(O_CLOEXEC | O_NONBLOCK)) != 0ULL) {
        return ERR_EINVAL;
    }

    const int read_fd = proc->fds().allocate(proc->rlim_nofile().rlim_cur, 0U);
    if (read_fd < 0) {
        return read_fd;
    }

    const uint32_t open_extra = ((flags & O_NONBLOCK) != 0ULL) ? Kernel::FS::kOpenNonBlock : 0U;
    const uint32_t fd_extra = ((flags & O_CLOEXEC) != 0ULL) ? Kernel::Process::FD_FLAG_CLOEXEC : 0U;

    Kernel::FS::File* read_file = nullptr;
    Kernel::FS::File* write_file = nullptr;
    const int32_t prc = Kernel::IPC::create_pipe_files(&read_file, &write_file, open_extra);
    if (prc != Kernel::FS::kOk) {
        return prc;
    }

    (void)proc->fds().install(static_cast<size_t>(read_fd), read_file, fd_extra);

    const int write_fd =
        proc->fds().allocate(proc->rlim_nofile().rlim_cur, static_cast<size_t>(read_fd) + 1U);
    if (write_fd < 0) {
        proc->fds().close(static_cast<size_t>(read_fd));
        (void)Kernel::FS::vfs_close(write_file);
        return write_fd;
    }

    (void)proc->fds().install(static_cast<size_t>(write_fd), write_file, fd_extra);

    const int32_t pair[2] = {read_fd, write_fd};
    if (Kernel::MM::copy_to_user(proc->address_space(), user_pipefd, pair, sizeof(pair)) < 0) {
        proc->fds().close(static_cast<size_t>(read_fd));
        proc->fds().close(static_cast<size_t>(write_fd));
        return ERR_EFAULT;
    }

    return 0;
}

// ==========================================================================
// TTY, Multiplexing & I/O Syscalls
// ==========================================================================

int64_t sys_ioctl(uint64_t raw_fd, uint64_t request, uintptr_t arg) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc != nullptr) {
        Kernel::FS::File* file = get_proc_file(proc, raw_fd);
        if (file == nullptr) {
            return ERR_EBADF;
        }
        return Kernel::FS::vfs_ioctl(file, request, static_cast<uint64_t>(arg));
    }

    if (raw_fd > 2U) {
        return ERR_EBADF;
    }
    Kernel::FS::File* console_file = nullptr;
    const int32_t orc = Kernel::FS::devfs_open_console(Kernel::FS::kOpenReadWrite, &console_file);
    if (orc != Kernel::FS::kOk) {
        return orc;
    }
    const int32_t res = Kernel::FS::vfs_ioctl(console_file, request, static_cast<uint64_t>(arg));
    (void)Kernel::FS::vfs_close(console_file);
    return res;
}

int64_t sys_read(uint64_t raw_fd, uintptr_t user_buf, uint64_t raw_count) noexcept {
    if (raw_count > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }
    const auto count = static_cast<size_t>(raw_count);
    auto* const proc = Kernel::Process::current_process();

    Kernel::FS::File* file = nullptr;
    bool temp_console = false;
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        file = get_proc_file(proc, raw_fd);
        if (file == nullptr) {
            return ERR_EBADF;
        }
        root_paddr = proc->address_space().root_paddr();
    } else {
        if (raw_fd != 0U) {
            return ERR_EBADF;
        }
        root_paddr = Kernel::MM::AddressSpace::current_root_paddr();
        if (Kernel::FS::devfs_open_console(Kernel::FS::kOpenReadOnly, &file) != Kernel::FS::kOk) {
            return ERR_EBADF;
        }
        temp_console = true;
    }

    if (!validate_user_write_range(root_paddr, user_buf, count)) {
        if (temp_console) {
            (void)Kernel::FS::vfs_close(file);
        }
        return ERR_EFAULT;
    }
    if (count == 0U) {
        const int64_t zero_res = Kernel::FS::vfs_read(file, nullptr, 0U);
        if (temp_console) {
            (void)Kernel::FS::vfs_close(file);
        }
        return (zero_res < 0) ? zero_res : 0;
    }

    char kbuf[READ_CHUNK_SIZE]{};
    size_t total_read = 0U;

    while (total_read < count) {
        const size_t remaining = count - total_read;
        const size_t chunk = (remaining < READ_CHUNK_SIZE) ? remaining : READ_CHUNK_SIZE;
        const int64_t nread = Kernel::FS::vfs_read(file, kbuf, chunk);
        if (nread < 0) {
            if (temp_console) {
                (void)Kernel::FS::vfs_close(file);
            }
            return (total_read > 0U) ? static_cast<int64_t>(total_read) : nread;
        }
        if (nread == 0) {
            break;
        }
        if (Kernel::MM::copy_to_user(root_paddr, user_buf + total_read, kbuf,
                                     static_cast<size_t>(nread)) < 0) {
            if (temp_console) {
                (void)Kernel::FS::vfs_close(file);
            }
            return (total_read > 0U) ? static_cast<int64_t>(total_read) : ERR_EFAULT;
        }
        total_read += static_cast<size_t>(nread);
        if (static_cast<size_t>(nread) < chunk) {
            break;
        }
    }

    if (temp_console) {
        (void)Kernel::FS::vfs_close(file);
    }
    return static_cast<int64_t>(total_read);
}

int64_t sys_write(uint64_t raw_fd, uintptr_t user_buf, uint64_t raw_count) noexcept {
    if (raw_count > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }
    const auto count = static_cast<size_t>(raw_count);
    auto* const proc = Kernel::Process::current_process();

    Kernel::FS::File* file = nullptr;
    bool temp_console = false;
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        file = get_proc_file(proc, raw_fd);
        if (file == nullptr) {
            return ERR_EBADF;
        }
        root_paddr = proc->address_space().root_paddr();
    } else {
        if (raw_fd != 1U && raw_fd != 2U) {
            return ERR_EBADF;
        }
        root_paddr = Kernel::MM::AddressSpace::current_root_paddr();
        if (Kernel::FS::devfs_open_console(Kernel::FS::kOpenWriteOnly, &file) != Kernel::FS::kOk) {
            return ERR_EBADF;
        }
        temp_console = true;
    }

    if (!validate_user_read_range(root_paddr, user_buf, count)) {
        if (temp_console) {
            (void)Kernel::FS::vfs_close(file);
        }
        return ERR_EFAULT;
    }
    if (count == 0U) {
        const int64_t zero_res = Kernel::FS::vfs_write(file, nullptr, 0U);
        if (temp_console) {
            (void)Kernel::FS::vfs_close(file);
        }
        return (zero_res < 0) ? zero_res : 0;
    }

    char kbuf[WRITE_CHUNK_SIZE]{};
    size_t total_written = 0U;

    while (total_written < count) {
        const size_t remaining = count - total_written;
        const size_t chunk = (remaining < WRITE_CHUNK_SIZE) ? remaining : WRITE_CHUNK_SIZE;
        if (Kernel::MM::copy_from_user(root_paddr, kbuf, user_buf + total_written, chunk) < 0) {
            if (temp_console) {
                (void)Kernel::FS::vfs_close(file);
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : ERR_EFAULT;
        }
        const int64_t nwritten = Kernel::FS::vfs_write(file, kbuf, chunk);
        if (nwritten < 0) {
            if (temp_console) {
                (void)Kernel::FS::vfs_close(file);
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : nwritten;
        }
        total_written += static_cast<size_t>(nwritten);
        if (static_cast<size_t>(nwritten) < chunk) {
            break;
        }
    }

    if (temp_console) {
        (void)Kernel::FS::vfs_close(file);
    }
    return static_cast<int64_t>(total_written);
}

int64_t sys_writev(uint64_t raw_fd, uintptr_t user_iov, uint64_t raw_iovcnt) noexcept {
    const auto fd_idx = static_cast<size_t>(raw_fd);
    auto* const proc = Kernel::Process::current_process();
    uintptr_t root_paddr = 0U;

    if (proc != nullptr) {
        const auto* const desc = proc->fds().at(fd_idx);
        if (desc == nullptr || !desc->is_open() || !desc->is_writable()) {
            return ERR_EBADF;
        }
        root_paddr = proc->address_space().root_paddr();
    } else {
        if (fd_idx != 1U && fd_idx != 2U) {
            return ERR_EBADF;
        }
        root_paddr = Kernel::MM::AddressSpace::current_root_paddr();
    }

    if (raw_iovcnt == 0U) {
        return 0;
    }
    if (raw_iovcnt > UIO_MAXIOV) {
        return ERR_EINVAL;
    }
    const auto iovcnt = static_cast<size_t>(raw_iovcnt);

    if (!validate_user_read_range(root_paddr, user_iov, iovcnt * sizeof(linux_iovec))) {
        return ERR_EFAULT;
    }

    uint64_t total_written = 0ULL;
    for (size_t idx = 0U; idx < iovcnt; ++idx) {
        linux_iovec iov{};
        const uintptr_t entry_vaddr = user_iov + (idx * sizeof(linux_iovec));
        if (Kernel::MM::copy_from_user(root_paddr, &iov, entry_vaddr, sizeof(linux_iovec)) < 0) {
            return (total_written > 0ULL) ? static_cast<int64_t>(total_written) : ERR_EFAULT;
        }
        if (iov.iov_len == 0ULL) {
            continue;
        }
        const int64_t wres = sys_write(raw_fd, static_cast<uintptr_t>(iov.iov_base), iov.iov_len);
        if (wres < 0) {
            return (total_written > 0ULL) ? static_cast<int64_t>(total_written) : wres;
        }
        total_written += static_cast<uint64_t>(wres);
    }

    return static_cast<int64_t>(total_written);
}

static int16_t poll_fd_revents(const Kernel::Process::Process& proc, int32_t raw_fd,
                               int16_t events) noexcept {
    if (raw_fd < 0) {
        return 0;
    }
    const auto* const desc = proc.fds().at(static_cast<size_t>(raw_fd));
    if (desc == nullptr || !desc->is_open()) {
        return POLLNVAL;
    }
    return Kernel::FS::vfs_poll(desc->file(), events);
}

int64_t sys_ppoll(uintptr_t user_fds, uint64_t nfds, uintptr_t user_tmo, uintptr_t user_sigmask,
                  uint64_t sigsetsize) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (nfds > proc->rlim_nofile().rlim_cur) {
        return ERR_EINVAL;
    }

    const uintptr_t root_paddr = proc->address_space().root_paddr();
    const auto count = static_cast<size_t>(nfds);

    linux_pollfd stack_fds[POLL_STACK_THRESHOLD]{};
    linux_pollfd* heap_fds = nullptr;
    linux_pollfd* kfdfs = stack_fds;

    struct PollHeapGuard {
        linux_pollfd* ptr{nullptr};
        explicit PollHeapGuard(linux_pollfd* heap_ptr) noexcept : ptr(heap_ptr) {}
        PollHeapGuard(const PollHeapGuard&) = delete;
        PollHeapGuard& operator=(const PollHeapGuard&) = delete;
        PollHeapGuard(PollHeapGuard&&) = delete;
        PollHeapGuard& operator=(PollHeapGuard&&) = delete;
        ~PollHeapGuard() noexcept { delete[] ptr; }
    };

    if (count > POLL_STACK_THRESHOLD) {
        heap_fds = new (std::nothrow) linux_pollfd[count]{};
        if (heap_fds == nullptr) {
            return ERR_ENOMEM;
        }
        kfdfs = heap_fds;
    }
    const PollHeapGuard heap_guard{heap_fds};

    if (count > 0U) {
        if (user_fds == 0U) {
            return ERR_EFAULT;
        }
        if (Kernel::MM::copy_from_user(root_paddr, kfdfs, user_fds, count * sizeof(linux_pollfd)) <
            0) {
            return ERR_EFAULT;
        }
    }

    bool has_timeout = false;
    bool zero_timeout = false;
    uint64_t deadline_ticks = 0ULL;
    if (user_tmo != 0U) {
        linux_timespec raw_tmo{};
        if (Kernel::MM::copy_from_user(root_paddr, &raw_tmo, user_tmo, sizeof(raw_tmo)) < 0) {
            return ERR_EFAULT;
        }
        Kernel::Time::Duration tmo_duration{};
        if (!Kernel::Time::Duration::from_linux_timespec(raw_tmo, tmo_duration)) {
            return ERR_EINVAL;
        }
        has_timeout = true;
        if (tmo_duration.is_zero()) {
            zero_timeout = true;
        } else {
            const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
            deadline_ticks =
                HAL::QemuVirt::GenericTimer::counter_ticks() + tmo_duration.to_timer_ticks(freq);
        }
    }

    bool swapped_mask = false;
    uint64_t saved_mask = 0ULL;
    if (user_sigmask != 0U) {
        if (sigsetsize != sizeof(uint64_t)) {
            return ERR_EINVAL;
        }
        uint64_t new_mask = 0ULL;
        if (Kernel::MM::copy_from_user(root_paddr, &new_mask, user_sigmask, sizeof(new_mask)) < 0) {
            return ERR_EFAULT;
        }
        saved_mask = proc->signals().swap_temp_mask(new_mask);
        swapped_mask = true;
    }

    auto restore_mask_normal = [&]() {
        if (swapped_mask) {
            proc->signals().set_blocked(saved_mask);
        }
    };

    auto restore_mask_eintr = [&]() {
        if (swapped_mask) {
            proc->signals().preserve_saved_mask_for_eintr(saved_mask);
        }
    };

    while (true) {
        int64_t ready_count = 0;
        for (size_t i = 0U; i < count; ++i) {
            kfdfs[i].revents = poll_fd_revents(*proc, kfdfs[i].fd, kfdfs[i].events);
            if (kfdfs[i].revents != 0) {
                ready_count++;
            }
        }

        if (ready_count > 0 || zero_timeout ||
            (has_timeout && HAL::QemuVirt::GenericTimer::counter_ticks() >= deadline_ticks)) {
            if (count > 0U) {
                if (Kernel::MM::copy_to_user(root_paddr, user_fds, kfdfs,
                                             count * sizeof(linux_pollfd)) < 0) {
                    restore_mask_normal();
                    return ERR_EFAULT;
                }
            }
            restore_mask_normal();
            return ready_count;
        }

        if (Kernel::Process::has_unblocked_pending_signal(*proc)) {
            restore_mask_eintr();
            return ERR_EINTR;
        }

        wait_for_event_or_tick();
    }
}

int64_t sys_pselect6(int32_t nfds, uintptr_t user_rfds, uintptr_t user_wfds, uintptr_t user_efds,
                     uintptr_t user_tmo, uintptr_t user_sigpack) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (nfds < 0 || static_cast<size_t>(nfds) > MAX_FDSET_FDS) {
        return ERR_EINVAL;
    }

    const uintptr_t root_paddr = proc->address_space().root_paddr();
    const auto u_nfds = static_cast<size_t>(nfds);
    const size_t nwords = (u_nfds + 63U) / 64U;
    const size_t set_bytes = nwords * sizeof(uint64_t);

    uint64_t rfds[MAX_FDSET_WORDS]{};
    uint64_t wfds[MAX_FDSET_WORDS]{};
    uint64_t efds[MAX_FDSET_WORDS]{};

    if (set_bytes > 0U) {
        if (user_rfds != 0U &&
            Kernel::MM::copy_from_user(root_paddr, rfds, user_rfds, set_bytes) < 0) {
            return ERR_EFAULT;
        }
        if (user_wfds != 0U &&
            Kernel::MM::copy_from_user(root_paddr, wfds, user_wfds, set_bytes) < 0) {
            return ERR_EFAULT;
        }
        if (user_efds != 0U &&
            Kernel::MM::copy_from_user(root_paddr, efds, user_efds, set_bytes) < 0) {
            return ERR_EFAULT;
        }
    }

    // Validate all requested file descriptors are open before blocking
    for (size_t fd = 0U; fd < u_nfds; ++fd) {
        const size_t word = fd / 64U;
        const uint64_t bit = 1ULL << (fd % 64U);
        const bool want_any = ((rfds[word] & bit) != 0ULL) || ((wfds[word] & bit) != 0ULL) ||
                              ((efds[word] & bit) != 0ULL);
        if (want_any) {
            const auto* const desc = proc->fds().at(fd);
            if (desc == nullptr || !desc->is_open()) {
                return ERR_EBADF;
            }
        }
    }

    bool has_timeout = false;
    bool zero_timeout = false;
    uint64_t deadline_ticks = 0ULL;
    if (user_tmo != 0U) {
        linux_timespec raw_tmo{};
        if (Kernel::MM::copy_from_user(root_paddr, &raw_tmo, user_tmo, sizeof(raw_tmo)) < 0) {
            return ERR_EFAULT;
        }
        Kernel::Time::Duration tmo_duration{};
        if (!Kernel::Time::Duration::from_linux_timespec(raw_tmo, tmo_duration)) {
            return ERR_EINVAL;
        }
        has_timeout = true;
        if (tmo_duration.is_zero()) {
            zero_timeout = true;
        } else {
            const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
            deadline_ticks =
                HAL::QemuVirt::GenericTimer::counter_ticks() + tmo_duration.to_timer_ticks(freq);
        }
    }

    bool swapped_mask = false;
    uint64_t saved_mask = 0ULL;
    if (user_sigpack != 0U) {
        linux_sigset_argpack pack{};
        if (Kernel::MM::copy_from_user(root_paddr, &pack, user_sigpack, sizeof(pack)) < 0) {
            return ERR_EFAULT;
        }
        if (pack.ss != 0ULL) {
            if (pack.ss_len != sizeof(uint64_t)) {
                return ERR_EINVAL;
            }
            uint64_t new_mask = 0ULL;
            if (Kernel::MM::copy_from_user(root_paddr, &new_mask, static_cast<uintptr_t>(pack.ss),
                                           sizeof(new_mask)) < 0) {
                return ERR_EFAULT;
            }
            saved_mask = proc->signals().swap_temp_mask(new_mask);
            swapped_mask = true;
        }
    }

    auto restore_mask_normal = [&]() {
        if (swapped_mask) {
            proc->signals().set_blocked(saved_mask);
        }
    };

    auto restore_mask_eintr = [&]() {
        if (swapped_mask) {
            proc->signals().preserve_saved_mask_for_eintr(saved_mask);
        }
    };

    constexpr auto READ_REQ =
        static_cast<int16_t>(static_cast<uint32_t>(POLLIN) | static_cast<uint32_t>(POLLRDNORM));
    constexpr auto READ_READY =
        static_cast<uint16_t>(static_cast<uint32_t>(POLLIN) | static_cast<uint32_t>(POLLRDNORM) |
                              static_cast<uint32_t>(POLLHUP) | static_cast<uint32_t>(POLLERR));
    constexpr auto WRITE_REQ =
        static_cast<int16_t>(static_cast<uint32_t>(POLLOUT) | static_cast<uint32_t>(POLLWRNORM));
    constexpr auto WRITE_READY =
        static_cast<uint16_t>(static_cast<uint32_t>(POLLOUT) | static_cast<uint32_t>(POLLWRNORM) |
                              static_cast<uint32_t>(POLLERR));
    constexpr auto ERR_READY = static_cast<uint16_t>(POLLERR);

    while (true) {
        uint64_t out_rfds[MAX_FDSET_WORDS]{};
        uint64_t out_wfds[MAX_FDSET_WORDS]{};
        uint64_t out_efds[MAX_FDSET_WORDS]{};
        int64_t ready_bits = 0;

        for (size_t fd = 0U; fd < u_nfds; ++fd) {
            const size_t word = fd / 64U;
            const uint64_t bit = 1ULL << (fd % 64U);

            if ((rfds[word] & bit) != 0ULL) {
                const auto rev = static_cast<uint16_t>(
                    poll_fd_revents(*proc, static_cast<int32_t>(fd), READ_REQ));
                if ((rev & READ_READY) != 0U) {
                    out_rfds[word] |= bit;
                    ready_bits++;
                }
            }
            if ((wfds[word] & bit) != 0ULL) {
                const auto rev = static_cast<uint16_t>(
                    poll_fd_revents(*proc, static_cast<int32_t>(fd), WRITE_REQ));
                if ((rev & WRITE_READY) != 0U) {
                    out_wfds[word] |= bit;
                    ready_bits++;
                }
            }
            if ((efds[word] & bit) != 0ULL) {
                const auto rev =
                    static_cast<uint16_t>(poll_fd_revents(*proc, static_cast<int32_t>(fd), 0));
                if ((rev & ERR_READY) != 0U) {
                    out_efds[word] |= bit;
                    ready_bits++;
                }
            }
        }

        if (ready_bits > 0 || zero_timeout ||
            (has_timeout && HAL::QemuVirt::GenericTimer::counter_ticks() >= deadline_ticks)) {
            if (set_bytes > 0U) {
                if (user_rfds != 0U &&
                    Kernel::MM::copy_to_user(root_paddr, user_rfds, out_rfds, set_bytes) < 0) {
                    restore_mask_normal();
                    return ERR_EFAULT;
                }
                if (user_wfds != 0U &&
                    Kernel::MM::copy_to_user(root_paddr, user_wfds, out_wfds, set_bytes) < 0) {
                    restore_mask_normal();
                    return ERR_EFAULT;
                }
                if (user_efds != 0U &&
                    Kernel::MM::copy_to_user(root_paddr, user_efds, out_efds, set_bytes) < 0) {
                    restore_mask_normal();
                    return ERR_EFAULT;
                }
            }
            restore_mask_normal();
            return ready_bits;
        }

        if (Kernel::Process::has_unblocked_pending_signal(*proc)) {
            restore_mask_eintr();
            return ERR_EINTR;
        }

        wait_for_event_or_tick();
    }
}

} // namespace Kernel::Syscalls
