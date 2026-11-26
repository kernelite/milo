#include "hal/cpu.hpp"
#include "hal/trap.hpp"
#include "process/scheduler.hpp"
#include "syscalls/SyscallInternal.hpp"

#include <cstdint>

namespace Kernel::Syscalls {

namespace {

// Linux AArch64 Syscall Numbers
constexpr uint64_t SYS_GETCWD = 17U;
constexpr uint64_t SYS_DUP = 23U;
constexpr uint64_t SYS_DUP3 = 24U;
constexpr uint64_t SYS_FCNTL = 25U;
constexpr uint64_t SYS_IOCTL = 29U;
constexpr uint64_t SYS_MKDIRAT = 34U;
constexpr uint64_t SYS_UNLINKAT = 35U;
constexpr uint64_t SYS_RENAMEAT = 38U;
constexpr uint64_t SYS_TRUNCATE = 45U;
constexpr uint64_t SYS_FTRUNCATE = 46U;
constexpr uint64_t SYS_FACCESSAT = 48U;
constexpr uint64_t SYS_CHDIR = 49U;
constexpr uint64_t SYS_FCHMOD = 52U;
constexpr uint64_t SYS_FCHMODAT = 53U;
constexpr uint64_t SYS_FCHOWNAT = 54U;
constexpr uint64_t SYS_FCHOWN = 55U;
constexpr uint64_t SYS_OPENAT = 56U;
constexpr uint64_t SYS_CLOSE = 57U;
constexpr uint64_t SYS_PIPE2 = 59U;
constexpr uint64_t SYS_GETDENTS64 = 61U;
constexpr uint64_t SYS_LSEEK = 62U;
constexpr uint64_t SYS_READ = 63U;
constexpr uint64_t SYS_WRITE = 64U;
constexpr uint64_t SYS_WRITEV = 66U;
constexpr uint64_t SYS_PSELECT6 = 72U;
constexpr uint64_t SYS_PPOLL = 73U;
constexpr uint64_t SYS_READLINKAT = 78U;
constexpr uint64_t SYS_NEWFSTATAT = 79U;
constexpr uint64_t SYS_FSTAT = 80U;
constexpr uint64_t SYS_FSYNC = 82U;
constexpr uint64_t SYS_FDATASYNC = 83U;
constexpr uint64_t SYS_UTIMENSAT = 88U;
constexpr uint64_t SYS_EXIT = 93U;
constexpr uint64_t SYS_EXIT_GROUP = 94U;
constexpr uint64_t SYS_SET_TID_ADDRESS = 96U;
constexpr uint64_t SYS_NANOSLEEP = 101U;
constexpr uint64_t SYS_CLOCK_GETTIME = 113U;
constexpr uint64_t SYS_SCHED_YIELD = 124U;
constexpr uint64_t SYS_KILL = 129U;
constexpr uint64_t SYS_RT_SIGACTION = 134U;
constexpr uint64_t SYS_RT_SIGPROCMASK = 135U;
constexpr uint64_t SYS_RT_SIGRETURN = 139U;
constexpr uint64_t SYS_TIMES = 153U;
constexpr uint64_t SYS_SETPGID = 154U;
constexpr uint64_t SYS_GETPGID = 155U;
constexpr uint64_t SYS_GETSID = 156U;
constexpr uint64_t SYS_SETSID = 157U;
constexpr uint64_t SYS_UNAME = 160U;
constexpr uint64_t SYS_GETRUSAGE = 165U;
constexpr uint64_t SYS_UMASK = 166U;
constexpr uint64_t SYS_GETPID = 172U;
constexpr uint64_t SYS_GETPPID = 173U;
constexpr uint64_t SYS_GETUID = 174U;
constexpr uint64_t SYS_GETEUID = 175U;
constexpr uint64_t SYS_GETGID = 176U;
constexpr uint64_t SYS_GETEGID = 177U;
constexpr uint64_t SYS_SYSINFO = 179U;
constexpr uint64_t SYS_BRK = 214U;
constexpr uint64_t SYS_MUNMAP = 215U;
constexpr uint64_t SYS_CLONE = 220U;
constexpr uint64_t SYS_EXECVE = 221U;
constexpr uint64_t SYS_MMAP = 222U;
constexpr uint64_t SYS_MPROTECT = 226U;
constexpr uint64_t SYS_WAIT4 = 260U;
constexpr uint64_t SYS_PRLIMIT64 = 261U;
constexpr uint64_t SYS_RENAMEAT2 = 276U;
constexpr uint64_t SYS_GETRANDOM = 278U;
constexpr uint64_t SYS_FACCESSAT2 = 439U;

} // namespace

class SyscallDispatcher : public HAL::TrapHandler {
  public:
    constexpr SyscallDispatcher() = default;

    void handle_trap(HAL::TrapFrame& frame) override {
        if (frame.type != HAL::TrapType::Syscall || frame.context == nullptr) {
            return;
        }

        auto& cpu = HAL::get_cpu();
        auto& ctx = *frame.context;
        const uint64_t syscall_num = cpu.get_syscall_number(ctx);
        int64_t ret_val = ERR_ENOSYS;

        switch (syscall_num) {
        case SYS_GETCWD:
            ret_val = sys_getcwd(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                 cpu.get_syscall_arg(ctx, 1U));
            break;

        case SYS_DUP:
            ret_val = sys_dup(cpu.get_syscall_arg(ctx, 0U));
            break;

        case SYS_DUP3:
            ret_val = sys_dup3(cpu.get_syscall_arg(ctx, 0U), cpu.get_syscall_arg(ctx, 1U),
                               cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_FCNTL:
            ret_val = sys_fcntl(cpu.get_syscall_arg(ctx, 0U), cpu.get_syscall_arg(ctx, 1U),
                                cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_IOCTL:
            ret_val = sys_ioctl(cpu.get_syscall_arg(ctx, 0U), cpu.get_syscall_arg(ctx, 1U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)));
            break;

        case SYS_FACCESSAT:
            ret_val = sys_faccessat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_FACCESSAT2:
            ret_val = sys_faccessat2(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_CHDIR:
            ret_val = sys_chdir(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_MKDIRAT:
            ret_val = sys_mkdirat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_UNLINKAT:
            ret_val = sys_unlinkat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_RENAMEAT:
            ret_val = sys_renameat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 2U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)));
            break;

        case SYS_RENAMEAT2:
            ret_val = sys_renameat2(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 2U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)), cpu.get_syscall_arg(ctx, 4U));
            break;

        case SYS_TRUNCATE:
            ret_val = sys_truncate(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                   static_cast<int64_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_FTRUNCATE:
            ret_val = sys_ftruncate(cpu.get_syscall_arg(ctx, 0U),
                                    static_cast<int64_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_FCHMOD:
            ret_val = sys_fchmod(cpu.get_syscall_arg(ctx, 0U), cpu.get_syscall_arg(ctx, 1U));
            break;

        case SYS_FCHMODAT:
            ret_val = sys_fchmodat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_FCHOWNAT:
            ret_val = sys_fchownat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 2U)),
                static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 3U)), cpu.get_syscall_arg(ctx, 4U));
            break;

        case SYS_FCHOWN:
            ret_val = sys_fchown(cpu.get_syscall_arg(ctx, 0U),
                                 static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 1U)),
                                 static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 2U)));
            break;

        case SYS_OPENAT:
            ret_val = sys_openat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_CLOSE:
            ret_val = sys_close(cpu.get_syscall_arg(ctx, 0U));
            break;

        case SYS_PIPE2:
            ret_val = sys_pipe2(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                cpu.get_syscall_arg(ctx, 1U));
            break;

        case SYS_GETDENTS64:
            ret_val = sys_getdents64(cpu.get_syscall_arg(ctx, 0U),
                                     static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                     cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_LSEEK:
            ret_val = sys_lseek(cpu.get_syscall_arg(ctx, 0U),
                                static_cast<int64_t>(cpu.get_syscall_arg(ctx, 1U)),
                                cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_READ:
            ret_val = sys_read(cpu.get_syscall_arg(ctx, 0U),
                               static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                               cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_WRITE:
            ret_val = sys_write(cpu.get_syscall_arg(ctx, 0U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_WRITEV:
            ret_val = sys_writev(cpu.get_syscall_arg(ctx, 0U),
                                 static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                 cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_PSELECT6:
            ret_val = sys_pselect6(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 4U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 5U)));
            break;

        case SYS_PPOLL:
            ret_val = sys_ppoll(
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)), cpu.get_syscall_arg(ctx, 1U),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)), cpu.get_syscall_arg(ctx, 4U));
            break;

        case SYS_READLINKAT:
            ret_val = sys_readlinkat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)), cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_NEWFSTATAT:
            ret_val = sys_newfstatat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)), cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_FSTAT:
            ret_val = sys_fstat(cpu.get_syscall_arg(ctx, 0U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_FSYNC:
            ret_val = sys_fsync(cpu.get_syscall_arg(ctx, 0U), 0);
            break;

        case SYS_FDATASYNC:
            ret_val = sys_fsync(cpu.get_syscall_arg(ctx, 0U), 1);
            break;

        case SYS_UTIMENSAT:
            ret_val = sys_utimensat(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)), cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            sys_exit(cpu.get_syscall_arg(ctx, 0U));

        case SYS_SET_TID_ADDRESS:
            ret_val = sys_set_tid_address(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_NANOSLEEP:
            ret_val = sys_nanosleep(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                    static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_CLOCK_GETTIME:
            ret_val = sys_clock_gettime(cpu.get_syscall_arg(ctx, 0U),
                                        static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_SCHED_YIELD:
            Kernel::Scheduler::yield();
            ret_val = 0;
            break;

        case SYS_KILL:
            ret_val =
                sys_kill(static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                         cpu.get_syscall_arg(ctx, 1U));
            break;

        case SYS_RT_SIGACTION:
            ret_val = sys_rt_sigaction(
                cpu.get_syscall_arg(ctx, 0U), static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)), cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_RT_SIGPROCMASK:
            ret_val = sys_rt_sigprocmask(
                cpu.get_syscall_arg(ctx, 0U), static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)), cpu.get_syscall_arg(ctx, 3U));
            break;

        case SYS_RT_SIGRETURN:
            ret_val = sys_rt_sigreturn(ctx);
            break;

        case SYS_TIMES:
            ret_val = sys_times(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_SETPGID:
            ret_val = sys_setpgid(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 1U))));
            break;

        case SYS_GETPGID:
            ret_val = sys_getpgid(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))));
            break;

        case SYS_GETSID:
            ret_val = sys_getsid(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))));
            break;

        case SYS_SETSID:
            ret_val = sys_setsid();
            break;

        case SYS_UNAME:
            ret_val = sys_uname(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_GETRUSAGE:
            ret_val = sys_getrusage(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)));
            break;

        case SYS_UMASK:
            ret_val = sys_umask(cpu.get_syscall_arg(ctx, 0U));
            break;

        case SYS_GETPID:
            ret_val = sys_getpid();
            break;

        case SYS_GETPPID:
            ret_val = sys_getppid();
            break;

        case SYS_GETUID:
        case SYS_GETEUID:
        case SYS_GETGID:
        case SYS_GETEGID:
            ret_val = 0;
            break;

        case SYS_SYSINFO:
            ret_val = sys_sysinfo(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_BRK:
            ret_val = sys_brk(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)));
            break;

        case SYS_MUNMAP:
            ret_val = sys_munmap(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                 cpu.get_syscall_arg(ctx, 1U));
            break;

        case SYS_MMAP:
            ret_val = sys_mmap(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                               cpu.get_syscall_arg(ctx, 1U), cpu.get_syscall_arg(ctx, 2U),
                               cpu.get_syscall_arg(ctx, 3U), cpu.get_syscall_arg(ctx, 4U),
                               cpu.get_syscall_arg(ctx, 5U));
            break;

        case SYS_MPROTECT:
            ret_val = sys_mprotect(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                   cpu.get_syscall_arg(ctx, 1U), cpu.get_syscall_arg(ctx, 2U));
            break;

        case SYS_CLONE:
            ret_val = sys_clone(ctx, cpu.get_syscall_arg(ctx, 0U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)),
                                cpu.get_syscall_arg(ctx, 3U),
                                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 4U)));
            break;

        case SYS_EXECVE:
            ret_val = sys_execve(ctx, static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                 static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)),
                                 static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)));
            if (ret_val == 0) {
                return;
            }
            break;

        case SYS_WAIT4:
            ret_val = sys_wait4(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 1U)), cpu.get_syscall_arg(ctx, 2U),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)));
            break;

        case SYS_PRLIMIT64:
            ret_val = sys_prlimit64(
                static_cast<int32_t>(static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 0U))),
                static_cast<uint32_t>(cpu.get_syscall_arg(ctx, 1U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 2U)),
                static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 3U)));
            break;

        case SYS_GETRANDOM:
            ret_val = sys_getrandom(static_cast<uintptr_t>(cpu.get_syscall_arg(ctx, 0U)),
                                    cpu.get_syscall_arg(ctx, 1U), cpu.get_syscall_arg(ctx, 2U));
            break;

        default:
            ret_val = ERR_ENOSYS;
            break;
        }

        cpu.set_syscall_return(ctx, ret_val);
    }
};

constinit SyscallDispatcher global_syscall_dispatcher{};

} // namespace Kernel::Syscalls

namespace HAL {
TrapHandler& get_trap_dispatcher() noexcept {
    return Kernel::Syscalls::global_syscall_dispatcher;
}
} // namespace HAL
