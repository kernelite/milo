#include "hal/cpu.hpp"
#include "syscalls/SyscallInternal.hpp"

namespace Kernel::Syscalls {

int64_t sys_rt_sigaction(uint64_t sig, uintptr_t user_act, uintptr_t user_oldact,
                         uint64_t sigsetsize) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (sigsetsize != sizeof(uint64_t) || sig == 0U || sig > Kernel::Process::MAX_SIGNALS) {
        return ERR_EINVAL;
    }

    const size_t idx = static_cast<size_t>(sig - 1U);
    if (user_oldact != 0U) {
        const auto& cur = proc->sigactions[idx];
        const LinuxSigAction out{
            .sa_handler = cur.handler,
            .sa_flags = cur.flags,
            .sa_restorer = cur.restorer,
            .sa_mask = cur.mask,
        };
        if (Kernel::MM::copy_to_user(proc->address_space, user_oldact, &out, sizeof(out)) < 0) {
            return ERR_EFAULT;
        }
    }

    if (user_act != 0U) {
        if (sig == Kernel::Process::SIG_KILL || sig == Kernel::Process::SIG_STOP) {
            return ERR_EINVAL;
        }
        LinuxSigAction dest{};
        if (Kernel::MM::copy_from_user(proc->address_space, &dest, user_act, sizeof(dest)) < 0) {
            return ERR_EFAULT;
        }
        proc->sigactions[idx] = Kernel::Process::SigAction{
            .handler = dest.sa_handler,
            .flags = dest.sa_flags,
            .restorer = dest.sa_restorer,
            .mask = dest.sa_mask,
        };
    }

    return 0;
}

int64_t sys_rt_sigprocmask(uint64_t how, uintptr_t user_set, uintptr_t user_oldset,
                           uint64_t sigsetsize) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr || sigsetsize != sizeof(uint64_t)) {
        return ERR_EINVAL;
    }

    if (user_oldset != 0U) {
        const uint64_t old_mask = proc->blocked_signals;
        if (Kernel::MM::copy_to_user(proc->address_space, user_oldset, &old_mask,
                                     sizeof(old_mask)) < 0) {
            return ERR_EFAULT;
        }
    }

    if (user_set != 0U) {
        uint64_t set_val = 0ULL;
        if (Kernel::MM::copy_from_user(proc->address_space, &set_val, user_set, sizeof(set_val)) <
            0) {
            return ERR_EFAULT;
        }
        constexpr uint64_t UNMASKABLE =
            (1ULL << (Kernel::Process::SIG_KILL - 1U)) | (1ULL << (Kernel::Process::SIG_STOP - 1U));
        set_val &= ~UNMASKABLE;

        if (how == SIG_BLOCK) {
            proc->blocked_signals |= set_val;
        } else if (how == SIG_UNBLOCK) {
            proc->blocked_signals &= ~set_val;
        } else if (how == SIG_SETMASK) {
            proc->blocked_signals = set_val;
        } else {
            return ERR_EINVAL;
        }
    }

    return 0;
}

int64_t sys_rt_sigreturn(HAL::CpuContext& ctx) noexcept {
    return Kernel::Process::restore_signal_frame(ctx);
}

int64_t sys_kill(int32_t pid, uint64_t sig) noexcept {
    auto* const caller = Kernel::Process::current_process();
    if (caller == nullptr) {
        return ERR_EINVAL;
    }
    if (sig > Kernel::Process::MAX_SIGNALS) {
        return ERR_EINVAL;
    }

    bool delivered_any = false;
    if (pid > 0) {
        auto* const target = Kernel::Process::find_process_by_pid(static_cast<uint32_t>(pid));
        if (target == nullptr || target->state == Kernel::Process::ProcessState::Zombie) {
            return ERR_ESRCH;
        }
        if (sig > 0U) {
            Kernel::Process::send_signal(*target, static_cast<uint32_t>(sig));
        }
        return 0;
    }

    const uint32_t target_pgid = (pid == 0) ? caller->pgid : static_cast<uint32_t>(-pid);
    for (auto* proc = Kernel::Process::process_list_head(); proc != nullptr;
         proc = proc->global_next) {
        if (proc->state == Kernel::Process::ProcessState::Unused ||
            proc->state == Kernel::Process::ProcessState::Zombie) {
            continue;
        }
        if (pid == -1) {
            if (proc->pid > 1U && proc != caller) {
                if (sig > 0U) {
                    Kernel::Process::send_signal(*proc, static_cast<uint32_t>(sig));
                }
                delivered_any = true;
            }
        } else if (proc->pgid == target_pgid) {
            if (sig > 0U) {
                Kernel::Process::send_signal(*proc, static_cast<uint32_t>(sig));
            }
            delivered_any = true;
        }
    }

    return delivered_any ? 0 : ERR_ESRCH;
}

} // namespace Kernel::Syscalls
