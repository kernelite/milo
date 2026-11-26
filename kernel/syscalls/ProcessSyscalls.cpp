#include "hal/cpu.hpp"
#include "loader/elf_loader.hpp"
#include "mm/pfa.hpp"
#include "process/resource.hpp"
#include "process/scheduler.hpp"
#include "process/stack_builder.hpp"
#include "syscalls/SyscallInternal.hpp"

namespace Kernel::Syscalls {

int64_t sys_setpgid(int32_t raw_pid, int32_t raw_pgid) noexcept {
    auto* const caller = Kernel::Process::Process::current();
    if (caller == nullptr || raw_pid < 0 || raw_pgid < 0) {
        return ERR_EINVAL;
    }

    const uint32_t target_pid = (raw_pid == 0) ? caller->pid() : static_cast<uint32_t>(raw_pid);
    auto* const target = Kernel::Process::Process::find_by_pid(target_pid);
    if (target == nullptr) {
        return ERR_ESRCH;
    }
    if (target != caller && target->ppid() != caller->pid()) {
        return ERR_ESRCH;
    }

    const uint32_t new_pgid = (raw_pgid == 0) ? target->pid() : static_cast<uint32_t>(raw_pgid);
    target->set_pgid(new_pgid);
    return 0;
}

int64_t sys_getpgid(int32_t raw_pid) noexcept {
    auto* const caller = Kernel::Process::Process::current();
    if (caller == nullptr || raw_pid < 0) {
        return ERR_EINVAL;
    }
    if (raw_pid == 0) {
        return static_cast<int64_t>(caller->pgid());
    }
    auto* const target = Kernel::Process::Process::find_by_pid(static_cast<uint32_t>(raw_pid));
    if (target == nullptr) {
        return ERR_ESRCH;
    }
    return static_cast<int64_t>(target->pgid());
}

int64_t sys_getsid(int32_t raw_pid) noexcept {
    auto* const caller = Kernel::Process::Process::current();
    if (caller == nullptr || raw_pid < 0) {
        return ERR_EINVAL;
    }
    if (raw_pid == 0) {
        return static_cast<int64_t>(caller->sid());
    }
    auto* const target = Kernel::Process::Process::find_by_pid(static_cast<uint32_t>(raw_pid));
    if (target == nullptr) {
        return ERR_ESRCH;
    }
    return static_cast<int64_t>(target->sid());
}

int64_t sys_setsid() noexcept {
    auto* const caller = Kernel::Process::Process::current();
    if (caller == nullptr) {
        return ERR_EINVAL;
    }
    if (!caller->setsid()) {
        return ERR_EPERM;
    }
    return static_cast<int64_t>(caller->sid());
}

[[noreturn]] void sys_exit(uint64_t raw_status) noexcept {
    const auto exit_code =
        static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(raw_status)));
    if (Kernel::Process::Process::current() != nullptr) {
        Kernel::Process::exit_current_process(exit_code);
    }
    HAL::get_cpu().return_to_kernel(exit_code);
    while (true) {
        HAL::get_cpu().halt();
    }
}

int64_t sys_getpid() noexcept {
    const auto* const proc = Kernel::Process::Process::current();
    return (proc != nullptr) ? static_cast<int64_t>(proc->pid()) : 0;
}

int64_t sys_getppid() noexcept {
    const auto* const proc = Kernel::Process::Process::current();
    return (proc != nullptr) ? static_cast<int64_t>(proc->ppid()) : 0;
}

int64_t sys_umask(uint64_t mask) noexcept {
    auto* const proc = Kernel::Process::Process::current();
    if (proc == nullptr) {
        return 0022;
    }
    return static_cast<int64_t>(proc->set_umask(static_cast<uint32_t>(mask)));
}

int64_t sys_prlimit64(int32_t pid, uint32_t resource, uintptr_t user_new_rlim,
                      uintptr_t user_old_rlim) noexcept {
    auto* const caller = Kernel::Process::Process::current();
    if (caller == nullptr) {
        return ERR_EINVAL;
    }
    if (pid < 0 || resource >= Kernel::Process::RLIM_NLIMITS) {
        return ERR_EINVAL;
    }

    Kernel::Process::Process* target = caller;
    if (pid > 0 && static_cast<uint32_t>(pid) != caller->pid()) {
        target = Kernel::Process::Process::find_by_pid(static_cast<uint32_t>(pid));
        if (target == nullptr || target->state() == Kernel::Process::ProcessState::Zombie) {
            return ERR_ESRCH;
        }
    }

    Kernel::Process::RLimit64 current_rlim{};

    switch (resource) {
    case Kernel::Process::RLIMIT_NOFILE:
        current_rlim = target->rlim_nofile();
        break;
    case Kernel::Process::RLIMIT_STACK:
        current_rlim.rlim_cur = Kernel::Process::DEFAULT_USER_STACK_SIZE;
        current_rlim.rlim_max = Kernel::Process::DEFAULT_USER_STACK_SIZE;
        break;
    case Kernel::Process::RLIMIT_NPROC:
        current_rlim = target->rlim_nproc();
        break;
    case Kernel::Process::RLIMIT_CORE:
        current_rlim.rlim_cur = 0ULL;
        current_rlim.rlim_max = 0ULL;
        break;
    default:
        break;
    }

    if (user_old_rlim != 0U) {
        const linux_rlimit64 raw_old = current_rlim.to_linux_rlimit64();
        if (Kernel::MM::copy_to_user(caller->address_space(), user_old_rlim, &raw_old,
                                     sizeof(raw_old)) < 0) {
            return ERR_EFAULT;
        }
    }

    if (user_new_rlim != 0U) {
        linux_rlimit64 raw_new{};
        if (Kernel::MM::copy_from_user(caller->address_space(), &raw_new, user_new_rlim,
                                       sizeof(raw_new)) < 0) {
            return ERR_EFAULT;
        }
        const Kernel::Process::RLimit64 new_rlim =
            Kernel::Process::RLimit64::from_linux_rlimit64(raw_new);
        if (!new_rlim.is_valid()) {
            return ERR_EINVAL;
        }
        if (resource == Kernel::Process::RLIMIT_NOFILE) {
            if (new_rlim.rlim_max > target->rlim_nofile().rlim_max ||
                new_rlim.rlim_max > Kernel::Process::DEFAULT_RLIMIT_NOFILE_MAX) {
                return ERR_EPERM;
            }
            target->set_rlim_nofile(new_rlim);
        } else if (resource == Kernel::Process::RLIMIT_NPROC) {
            if (new_rlim.rlim_max > target->rlim_nproc().rlim_max) {
                return ERR_EPERM;
            }
            target->set_rlim_nproc(new_rlim);
        }
    }

    return 0;
}

int64_t sys_clone(HAL::CpuContext& parent_ctx, uint64_t flags, uintptr_t child_stack,
                  uintptr_t parent_tid, uint64_t tls, uintptr_t child_tid) noexcept {
    (void)tls;

    auto* const parent = Kernel::Process::Process::current();
    if (parent == nullptr) {
        return ERR_EINVAL;
    }

    constexpr uint64_t UNSUPPORTED_CLONE_FLAGS =
        CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_VFORK | CLONE_THREAD;
    constexpr uint64_t ALLOWED_CLONE_FLAGS =
        CSIGNAL_MASK | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;

    if ((flags & UNSUPPORTED_CLONE_FLAGS) != 0ULL || (flags & ~ALLOWED_CLONE_FLAGS) != 0ULL) {
        return ERR_EINVAL;
    }

    const uint64_t exit_sig = flags & CSIGNAL_MASK;
    if (exit_sig != 0ULL && exit_sig != SIGCHLD_NUM) {
        return ERR_EINVAL;
    }

    if (child_stack != 0U && child_stack >= Kernel::MM::user_addr_space_limit()) {
        return ERR_EINVAL;
    }

    if (Kernel::Process::Process::active_count() >= parent->rlim_nproc().rlim_cur) {
        return ERR_EAGAIN;
    }

    auto* const child = Kernel::Process::Process::create(parent->pid());
    if (child == nullptr) {
        return ERR_EAGAIN;
    }

    auto clone_as_res = parent->address_space().clone();
    if (!clone_as_res.is_ok()) {
        Kernel::Process::Process::destroy(*child);
        return ERR_ENOMEM;
    }

    child->address_space() = static_cast<Kernel::MM::AddressSpace&&>(clone_as_res.value());
    child->set_pgid(parent->pgid());
    child->set_sid(parent->sid());
    (void)child->set_umask(parent->umask());
    child->set_rlim_nofile(parent->rlim_nofile());
    child->set_rlim_nproc(parent->rlim_nproc());
    child->set_entry_point(parent->entry_point());
    child->set_brk_start(parent->brk_start());
    child->set_brk_current(parent->brk_current());
    child->set_mmap_next_vaddr(parent->mmap_next_vaddr());
    child->set_user_stack(Kernel::Process::UserStack{});
    child->signals().clone_from(parent->signals());
    child->set_cwd(parent->cwd());
    child->set_exe_path(parent->exe_path());

    if (!child->fds().clone_from(parent->fds())) {
        Kernel::Process::Process::destroy(*child);
        return ERR_ENOMEM;
    }

    const auto child_pid_s32 = static_cast<int32_t>(child->pid());
    if ((flags & CLONE_PARENT_SETTID) != 0ULL && parent_tid != 0U) {
        const int64_t tid_res = Kernel::MM::copy_to_user(parent->address_space(), parent_tid,
                                                         &child_pid_s32, sizeof(child_pid_s32));
        if (tid_res < 0) {
            Kernel::Process::Process::destroy(*child);
            return ERR_EFAULT;
        }
    }

    if ((flags & CLONE_CHILD_SETTID) != 0ULL && child_tid != 0U) {
        const int64_t tid_res = Kernel::MM::copy_to_user(child->address_space(), child_tid,
                                                         &child_pid_s32, sizeof(child_pid_s32));
        if (tid_res < 0) {
            Kernel::Process::Process::destroy(*child);
            return ERR_EFAULT;
        }
    }

    auto& cpu = HAL::get_cpu();
    child->context() = parent_ctx;
    if (child_stack != 0U) {
        cpu.set_user_stack_pointer(child->context(), child_stack);
    }
    cpu.set_syscall_return(child->context(), 0);
    child->set_kctx_initialized(false);
    Kernel::Scheduler::enqueue(child);

    return static_cast<int64_t>(child->pid());
}

int64_t sys_execve(HAL::CpuContext& ctx, uintptr_t user_pathname, uintptr_t user_argv,
                   uintptr_t user_envp) noexcept {
    auto* const proc = Kernel::Process::Process::current();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uintptr_t scratch_paddr = Kernel::MM::alloc_frame();
    if (scratch_paddr == 0U) {
        return ERR_ENOMEM;
    }

    auto* const scratch_kva = reinterpret_cast<char*>(Kernel::MM::phys_to_virt(scratch_paddr));
    char* const raw_path_buf = scratch_kva;
    char* const string_pool = scratch_kva + EXEC_MAX_PATH_LEN;
    const size_t string_pool_cap = page_size - EXEC_MAX_PATH_LEN;

    const char* argv_kptrs[EXEC_MAX_ARGS + 3U]{};
    const char* envp_kptrs[EXEC_MAX_ENVS + 1U]{};

    const uintptr_t caller_root = proc->address_space().root_paddr();
    size_t path_len = 0U;
    const int64_t path_res =
        copy_user_cstring(caller_root, user_pathname, raw_path_buf, EXEC_MAX_PATH_LEN, path_len);
    if (path_res < 0) {
        Kernel::MM::free_frame(scratch_paddr);
        return path_res;
    }

    char canon_path[Kernel::Process::MAX_PATH_LEN]{};
    if (!normalize_vfs_path(proc->cwd(), raw_path_buf, canon_path, sizeof(canon_path))) {
        Kernel::MM::free_frame(scratch_paddr);
        return ERR_ENOENT;
    }

    size_t pool_offset = 0U;
    int argc = 0;
    const int64_t argv_res =
        stage_user_string_array(caller_root, user_argv, argv_kptrs, EXEC_MAX_ARGS, string_pool,
                                string_pool_cap, pool_offset, argc);
    if (argv_res < 0) {
        Kernel::MM::free_frame(scratch_paddr);
        return argv_res;
    }

    int envc = 0;
    const int64_t envp_res =
        stage_user_string_array(caller_root, user_envp, envp_kptrs, EXEC_MAX_ENVS, string_pool,
                                string_pool_cap, pool_offset, envc);
    if (envp_res < 0) {
        Kernel::MM::free_frame(scratch_paddr);
        return envp_res;
    }
    (void)envc;

    auto open_and_read_exec = [&](const char* path, uintptr_t& out_buf_paddr, size_t& out_num_pages,
                                  size_t& out_file_size) -> int64_t {
        out_buf_paddr = 0U;
        out_num_pages = 0U;
        out_file_size = 0U;

        Kernel::FS::File* exec_file = nullptr;
        const int32_t orc = Kernel::FS::vfs_open(Kernel::FS::StringView(path),
                                                 Kernel::FS::kOpenReadOnly, 0U, &exec_file);
        if (orc != Kernel::FS::kOk) {
            return orc;
        }
        if (exec_file->inode == nullptr || !exec_file->inode->is_reg() ||
            exec_file->inode->size == 0U) {
            (void)Kernel::FS::vfs_close(exec_file);
            return ERR_ENOEXEC;
        }

        const auto file_size = static_cast<size_t>(exec_file->inode->size);
        const size_t num_pages = (file_size + page_size - 1U) / page_size;
        const uintptr_t buf_paddr = Kernel::MM::alloc_frames(num_pages);
        if (buf_paddr == 0U) {
            (void)Kernel::FS::vfs_close(exec_file);
            return ERR_ENOMEM;
        }

        auto* const dst = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(buf_paddr));
        size_t total = 0U;
        while (total < file_size) {
            const int64_t nread = Kernel::FS::vfs_read(exec_file, dst + total, file_size - total);
            if (nread <= 0) {
                Kernel::MM::free_frames(buf_paddr, num_pages);
                (void)Kernel::FS::vfs_close(exec_file);
                return (nread < 0) ? nread : ERR_ENOEXEC;
            }
            total += static_cast<size_t>(nread);
        }
        (void)Kernel::FS::vfs_close(exec_file);

        out_buf_paddr = buf_paddr;
        out_num_pages = num_pages;
        out_file_size = file_size;
        return 0;
    };

    uintptr_t exec_buf_paddr = 0U;
    size_t exec_buf_pages = 0U;
    size_t exec_file_size = 0U;
    const int64_t read_exec_res =
        open_and_read_exec(canon_path, exec_buf_paddr, exec_buf_pages, exec_file_size);
    if (read_exec_res < 0) {
        Kernel::MM::free_frame(scratch_paddr);
        return read_exec_res;
    }

    const auto* exec_bytes =
        reinterpret_cast<const uint8_t*>(Kernel::MM::phys_to_virt(exec_buf_paddr));

    // Handle '#!' shebang scripts (e.g., #!/bin/bash or #!/bin/sh)
    char shebang_interp[Kernel::Process::MAX_PATH_LEN]{};
    char shebang_arg[Kernel::Process::MAX_PATH_LEN]{};
    bool has_shebang_arg = false;
    char exec_binary_path[Kernel::Process::MAX_PATH_LEN]{};
    cstr_copy(exec_binary_path, canon_path, sizeof(exec_binary_path));

    if (exec_file_size >= 2U && exec_bytes[0] == '#' && exec_bytes[1] == '!') {
        size_t pos = 2U;
        while (pos < exec_file_size && (exec_bytes[pos] == ' ' || exec_bytes[pos] == '\t')) {
            pos++;
        }
        size_t interp_len = 0U;
        while (pos < exec_file_size && exec_bytes[pos] != ' ' && exec_bytes[pos] != '\t' &&
               exec_bytes[pos] != '\n' && exec_bytes[pos] != '\r') {
            if (interp_len + 1U >= sizeof(shebang_interp)) {
                Kernel::MM::free_frames(exec_buf_paddr, exec_buf_pages);
                Kernel::MM::free_frame(scratch_paddr);
                return ERR_ENOEXEC;
            }
            shebang_interp[interp_len++] = static_cast<char>(exec_bytes[pos++]);
        }
        shebang_interp[interp_len] = '\0';
        if (interp_len == 0U) {
            Kernel::MM::free_frames(exec_buf_paddr, exec_buf_pages);
            Kernel::MM::free_frame(scratch_paddr);
            return ERR_ENOEXEC;
        }

        while (pos < exec_file_size && (exec_bytes[pos] == ' ' || exec_bytes[pos] == '\t')) {
            pos++;
        }
        size_t arg_len = 0U;
        while (pos < exec_file_size && exec_bytes[pos] != '\n' && exec_bytes[pos] != '\r') {
            if (arg_len + 1U < sizeof(shebang_arg)) {
                shebang_arg[arg_len++] = static_cast<char>(exec_bytes[pos]);
            }
            pos++;
        }
        while (arg_len > 0U &&
               (shebang_arg[arg_len - 1U] == ' ' || shebang_arg[arg_len - 1U] == '\t')) {
            arg_len--;
        }
        shebang_arg[arg_len] = '\0';
        has_shebang_arg = (arg_len > 0U);

        Kernel::MM::free_frames(exec_buf_paddr, exec_buf_pages);
        exec_buf_paddr = 0U;

        if (!normalize_vfs_path("/", shebang_interp, exec_binary_path, sizeof(exec_binary_path))) {
            Kernel::MM::free_frame(scratch_paddr);
            return ERR_ENOENT;
        }
        const int64_t interp_res =
            open_and_read_exec(exec_binary_path, exec_buf_paddr, exec_buf_pages, exec_file_size);
        if (interp_res < 0) {
            Kernel::MM::free_frame(scratch_paddr);
            return interp_res;
        }
        exec_bytes = reinterpret_cast<const uint8_t*>(Kernel::MM::phys_to_virt(exec_buf_paddr));

        const int extra = has_shebang_arg ? 2 : 1;
        if (static_cast<size_t>(argc) + static_cast<size_t>(extra) > EXEC_MAX_ARGS) {
            Kernel::MM::free_frames(exec_buf_paddr, exec_buf_pages);
            Kernel::MM::free_frame(scratch_paddr);
            return ERR_E2BIG;
        }
        for (int idx = argc; idx >= 1; --idx) {
            argv_kptrs[idx + extra] = argv_kptrs[idx];
        }
        argv_kptrs[0] = exec_binary_path;
        if (has_shebang_arg) {
            argv_kptrs[1] = shebang_arg;
            argv_kptrs[2] = canon_path;
        } else {
            argv_kptrs[1] = canon_path;
        }
        argc += extra;
    }

    auto load_res = Kernel::Loader::load_elf(exec_bytes, exec_file_size);
    Kernel::MM::free_frames(exec_buf_paddr, exec_buf_pages);
    if (!load_res.is_ok()) {
        Kernel::MM::free_frame(scratch_paddr);
        return (load_res.error() == Kernel::Loader::ElfLoaderError::OutOfMemory) ? ERR_ENOMEM
                                                                                 : ERR_ENOEXEC;
    }

    auto loaded = static_cast<Kernel::Loader::LoadedElf&&>(load_res.value());
    auto stack_res = Kernel::Process::UserStackBuilder::create_and_map(loaded.address_space);
    if (!stack_res.is_ok()) {
        loaded.address_space.destroy();
        Kernel::MM::free_frame(scratch_paddr);
        return ERR_ENOMEM;
    }

    auto new_stack = stack_res.value();
    Kernel::Loader::Elf64_auxv_t auxv[Kernel::Process::STANDARD_AUXV_COUNT]{};
    const size_t auxv_count =
        Kernel::Process::populate_standard_auxv(loaded, auxv, Kernel::Process::STANDARD_AUXV_COUNT);

    auto abi_res = Kernel::Process::UserStackBuilder::build_abi_layout(
        new_stack, argc, argv_kptrs, envp_kptrs, auxv, auxv_count);
    if (!abi_res.is_ok()) {
        if (!loaded.address_space.is_valid() && new_stack.phys_base != 0U &&
            new_stack.num_pages > 0U) {
            Kernel::MM::free_frames(new_stack.phys_base, new_stack.num_pages);
        }
        loaded.address_space.destroy();
        Kernel::MM::free_frame(scratch_paddr);
        return ERR_E2BIG;
    }

    if (!proc->fds().is_initialized() && !proc->fds().init(Kernel::Process::INITIAL_FD_CAPACITY)) {
        if (!loaded.address_space.is_valid() && new_stack.phys_base != 0U &&
            new_stack.num_pages > 0U) {
            Kernel::MM::free_frames(new_stack.phys_base, new_stack.num_pages);
        }
        loaded.address_space.destroy();
        Kernel::MM::free_frame(scratch_paddr);
        return ERR_ENOMEM;
    }

    loaded.address_space.activate();

    if (!proc->address_space().is_valid() && proc->user_stack().phys_base != 0U &&
        proc->user_stack().num_pages > 0U) {
        Kernel::MM::free_frames(proc->user_stack().phys_base, proc->user_stack().num_pages);
    }
    proc->address_space() = static_cast<Kernel::MM::AddressSpace&&>(loaded.address_space);
    proc->set_exe_path(exec_binary_path);

    proc->fds().close_cloexec();
    proc->signals().reset_caught_for_exec();

    const auto platform = HAL::get_platform_info();
    proc->set_entry_point(loaded.entry_point);
    proc->set_user_stack(new_stack);
    const uintptr_t new_brk = (loaded.max_vaddr != 0U) ? loaded.max_vaddr : platform.user_brk_base;
    proc->set_brk_start(new_brk);
    proc->set_brk_current(new_brk);
    proc->set_mmap_next_vaddr(platform.user_mmap_base);

    Kernel::Process::init_user_cpu_context(ctx, proc->entry_point(),
                                           proc->user_stack().final_sp_el0, proc->user_stack().argc,
                                           proc->user_stack().argv_vaddr);

    Kernel::MM::free_frame(scratch_paddr);
    return 0;
}

int64_t sys_wait4(int32_t target_pid, uintptr_t user_wstatus, uint64_t options,
                  uintptr_t user_rusage) noexcept {
    auto* const parent = Kernel::Process::Process::current();
    if (parent == nullptr) {
        return ERR_EINVAL;
    }

    constexpr uint64_t ALLOWED_WAIT_OPTIONS = WAIT_WNOHANG | WAIT_WUNTRACED | WAIT_WCONTINUED;
    if ((options & ~ALLOWED_WAIT_OPTIONS) != 0ULL) {
        return ERR_EINVAL;
    }

    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();

    Kernel::Process::Process* target_child = nullptr;
    int32_t encoded_wstatus = 0;
    bool reap_child = false;

    while (true) {
        cpu.disable_interrupts();

        bool has_matching_child = false;
        Kernel::Process::Process* ready_child = nullptr;

        for (auto* candidate = Kernel::Process::Process::list_head(); candidate != nullptr;
             candidate = candidate->next_global()) {
            if (candidate->state() == Kernel::Process::ProcessState::Unused) {
                continue;
            }
            if (candidate->ppid() != parent->pid()) {
                continue;
            }
            if (target_pid > 0 && candidate->pid() != static_cast<uint32_t>(target_pid)) {
                continue;
            }
            if (target_pid == 0 && candidate->pgid() != parent->pgid()) {
                continue;
            }
            if (target_pid < -1 && candidate->pgid() != static_cast<uint32_t>(-target_pid)) {
                continue;
            }

            has_matching_child = true;
            if (candidate->state() == Kernel::Process::ProcessState::Zombie) {
                ready_child = candidate;
                encoded_wstatus = static_cast<int32_t>(
                    (static_cast<uint32_t>(candidate->exit_status()) & 0xFFU) << 8U);
                reap_child = true;
                break;
            }
            uint32_t stopped_sig = 0U;
            if ((options & WAIT_WUNTRACED) != 0ULL && candidate->consume_stop_report(stopped_sig)) {
                ready_child = candidate;
                encoded_wstatus = static_cast<int32_t>(((stopped_sig & 0xFFU) << 8U) | 0x7FU);
                reap_child = false;
                break;
            }
            if ((options & WAIT_WCONTINUED) != 0ULL && candidate->consume_continued()) {
                ready_child = candidate;
                encoded_wstatus = 0xFFFF;
                reap_child = false;
                break;
            }
        }

        if (!has_matching_child) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return ERR_ECHILD;
        }

        if (ready_child != nullptr) {
            target_child = ready_child;
            break;
        }

        if ((options & WAIT_WNOHANG) != 0ULL) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return 0;
        }

        if (parent->signals().has_unblocked_pending()) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return ERR_EINTR;
        }

        parent->child_wait_queue().sleep();
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }

    if (user_wstatus != 0U) {
        const int64_t status_res = Kernel::MM::copy_to_user(
            parent->address_space(), user_wstatus, &encoded_wstatus, sizeof(encoded_wstatus));
        if (status_res < 0) {
            return ERR_EFAULT;
        }
    }

    if (user_rusage != 0U) {
        linux_rusage rusage{};
        const uint64_t child_user_usec =
            static_cast<uint64_t>(target_child->utime() + target_child->cutime()) * 10000ULL;
        const uint64_t child_sys_usec =
            static_cast<uint64_t>(target_child->stime() + target_child->cstime()) * 10000ULL;
        rusage.ru_utime =
            Kernel::Time::Duration::from_microseconds(child_user_usec).to_linux_timeval();
        rusage.ru_stime =
            Kernel::Time::Duration::from_microseconds(child_sys_usec).to_linux_timeval();
        const int64_t rusage_res =
            Kernel::MM::copy_to_user(parent->address_space(), user_rusage, &rusage, sizeof(rusage));
        if (rusage_res < 0) {
            return ERR_EFAULT;
        }
    }

    const uint32_t reported_pid = target_child->pid();
    if (reap_child) {
        parent->accumulate_child_ticks(*target_child);
        Kernel::Process::Process::destroy(*target_child);
    }
    return static_cast<int64_t>(reported_pid);
}

int64_t sys_set_tid_address(uintptr_t tidptr) noexcept {
    auto* const proc = Kernel::Process::Process::current();
    if (proc == nullptr) {
        return 0;
    }
    proc->set_clear_child_tid(tidptr);
    return static_cast<int64_t>(proc->pid());
}

} // namespace Kernel::Syscalls
