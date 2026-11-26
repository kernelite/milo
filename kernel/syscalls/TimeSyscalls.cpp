#include "hal/cpu.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "mm/pfa.hpp"
#include "process/scheduler.hpp"
#include "syscalls/SyscallInternal.hpp"

namespace Kernel::Syscalls {

int64_t sys_getrusage(int32_t who, uintptr_t user_usage) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) {
        return ERR_EINVAL;
    }

    const uint64_t uticks = (who == RUSAGE_CHILDREN) ? proc->cutime : proc->utime;
    const uint64_t sticks = (who == RUSAGE_CHILDREN) ? proc->cstime : proc->stime;
    // Timer runs at 100 Hz (10,000 microseconds per tick)
    const uint64_t user_usec = uticks * 10000ULL;
    const uint64_t sys_usec = sticks * 10000ULL;

    LinuxRusage usage{};
    usage.ru_utime.tv_sec = static_cast<int64_t>(user_usec / 1000000ULL);
    usage.ru_utime.tv_usec = static_cast<int64_t>(user_usec % 1000000ULL);
    usage.ru_stime.tv_sec = static_cast<int64_t>(sys_usec / 1000000ULL);
    usage.ru_stime.tv_usec = static_cast<int64_t>(sys_usec % 1000000ULL);

    return Kernel::MM::copy_to_user(proc->address_space, user_usage, &usage, sizeof(usage));
}

int64_t sys_getrandom(uintptr_t user_buf, uint64_t raw_buflen, uint64_t flags) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }
    if ((flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) != 0ULL) {
        return ERR_EINVAL;
    }
    if (raw_buflen > Kernel::MM::user_addr_space_limit()) {
        return ERR_EFAULT;
    }

    const auto buflen = static_cast<size_t>(raw_buflen);
    const uintptr_t root_paddr = proc->address_space.root_paddr();
    if (!validate_user_write_range(root_paddr, user_buf, buflen)) {
        return ERR_EFAULT;
    }
    if (buflen == 0U) {
        return 0;
    }

    static uint64_t entropy_state = 0x243F6A8885A308D3ULL;
    entropy_state ^= HAL::QemuVirt::GenericTimer::counter_ticks();
    entropy_state ^= static_cast<uint64_t>(proc->pid) << 32U;

    uint8_t kbuf[256]{};
    size_t written = 0U;

    while (written < buflen) {
        const size_t chunk =
            ((buflen - written) < sizeof(kbuf)) ? (buflen - written) : sizeof(kbuf);
        for (size_t i = 0U; i < chunk; i += 8U) {
            entropy_state += 0x9E3779B97F4A7C15ULL;
            uint64_t mixed = entropy_state;
            mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
            mixed = mixed ^ (mixed >> 31U);

            const size_t rem = chunk - i;
            const size_t bytes = (rem < 8U) ? rem : 8U;
            for (size_t byte_idx = 0U; byte_idx < bytes; ++byte_idx) {
                kbuf[i + byte_idx] = static_cast<uint8_t>((mixed >> (byte_idx * 8U)) & 0xFFULL);
            }
        }

        if (Kernel::MM::copy_to_user(root_paddr, user_buf + written, kbuf, chunk) < 0) {
            return (written > 0U) ? static_cast<int64_t>(written) : ERR_EFAULT;
        }
        written += chunk;
    }

    return static_cast<int64_t>(written);
}

int64_t sys_nanosleep(uintptr_t user_req, uintptr_t user_rem) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    LinuxTimespec req{};
    if (Kernel::MM::copy_from_user(proc->address_space, &req, user_req, sizeof(req)) < 0) {
        return ERR_EFAULT;
    }
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000LL) {
        return ERR_EINVAL;
    }

    const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
    const uint64_t start_ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
    const uint64_t wait_ticks = timespec_to_timer_ticks(req, freq);

    while (true) {
        const uint64_t now_ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
        const uint64_t elapsed = now_ticks - start_ticks;
        if (elapsed >= wait_ticks) {
            break;
        }

        if (Kernel::Process::has_unblocked_pending_signal(*proc)) {
            if (user_rem != 0U && freq > 0ULL) {
                const uint64_t rem_ticks = wait_ticks - elapsed;
                const LinuxTimespec rem{
                    .tv_sec = static_cast<int64_t>(rem_ticks / freq),
                    .tv_nsec = static_cast<int64_t>(((rem_ticks % freq) * 1000000000ULL) / freq),
                };
                if (Kernel::MM::copy_to_user(proc->address_space, user_rem, &rem, sizeof(rem)) <
                    0) {
                    return ERR_EFAULT;
                }
            }
            return ERR_EINTR;
        }

        wait_for_event_or_tick();
    }

    if (user_rem != 0U) {
        constexpr LinuxTimespec zero_rem{.tv_sec = 0, .tv_nsec = 0};
        if (Kernel::MM::copy_to_user(proc->address_space, user_rem, &zero_rem, sizeof(zero_rem)) <
            0) {
            return ERR_EFAULT;
        }
    }

    return 0;
}

int64_t sys_times(uintptr_t user_buf) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    if (user_buf != 0U) {
        const LinuxTms tms_val{
            .tms_utime = static_cast<int64_t>(proc->utime),
            .tms_stime = static_cast<int64_t>(proc->stime),
            .tms_cutime = static_cast<int64_t>(proc->cutime),
            .tms_cstime = static_cast<int64_t>(proc->cstime),
        };
        if (Kernel::MM::copy_to_user(proc->address_space, user_buf, &tms_val, sizeof(tms_val)) <
            0) {
            return ERR_EFAULT;
        }
    }

    return static_cast<int64_t>(Kernel::Scheduler::timer_ticks());
}

int64_t sys_uname(uintptr_t user_buf) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    LinuxUtsname uts{};
    cstr_copy(uts.sysname, "Linux", sizeof(uts.sysname));
    cstr_copy(uts.nodename, "milo", sizeof(uts.nodename));
    cstr_copy(uts.release, "6.6.0-milo", sizeof(uts.release));
    cstr_copy(uts.version, "#1 SMP PREEMPT", sizeof(uts.version));
    cstr_copy(uts.machine, "aarch64", sizeof(uts.machine));
    cstr_copy(uts.domainname, "(none)", sizeof(uts.domainname));

    return Kernel::MM::copy_to_user(proc->address_space, user_buf, &uts, sizeof(uts));
}

int64_t sys_clock_gettime(uint64_t clk_id, uintptr_t user_tp) noexcept {
    (void)clk_id;
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
    const uint64_t ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
    LinuxTimespec time_spec{};
    if (freq > 0ULL) {
        time_spec.tv_sec = static_cast<int64_t>(ticks / freq);
        const uint64_t rem = ticks % freq;
        time_spec.tv_nsec = static_cast<int64_t>((rem * 1000000000ULL) / freq);
    }

    return Kernel::MM::copy_to_user(proc->address_space, user_tp, &time_spec, sizeof(time_spec));
}

int64_t sys_sysinfo(uintptr_t user_info) noexcept {
    auto* const proc = Kernel::Process::current_process();
    if (proc == nullptr) {
        return ERR_EINVAL;
    }

    const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
    const uint64_t ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
    const auto platform = HAL::get_platform_info();

    LinuxSysinfo info{};
    info.uptime = (freq > 0ULL) ? static_cast<int64_t>(ticks / freq) : 0;
    info.totalram = platform.ram_size;
    info.freeram = Kernel::MM::pfa_free_pages_count() * platform.page_size;
    info.procs = static_cast<uint16_t>(Kernel::Process::active_process_count());
    info.mem_unit = 1U;

    return Kernel::MM::copy_to_user(proc->address_space, user_info, &info, sizeof(info));
}

} // namespace Kernel::Syscalls
