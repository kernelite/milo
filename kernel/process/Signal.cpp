#include "process/Signal.hpp"

#include "hal/cpu.hpp"
#include "mm/AddressSpace.hpp"
#include "process/Process.hpp"
#include "process/scheduler.hpp"
#include "sync/IrqGuard.hpp"
#include "uapi/signal.hpp"

namespace Kernel::Process {

namespace {

bool is_stop_signal(uint32_t sig) noexcept {
    return sig == SIG_STOP || sig == SIG_TSTP || sig == SIG_TTIN || sig == SIG_TTOU;
}

} // namespace

void SignalTable::pend(uint32_t sig) noexcept {
    if (is_valid_signal(sig)) {
        m_pending |= sig_bit(sig);
    }
}

void SignalTable::clear_stop_signals() noexcept {
    m_pending &= ~STOP_SIGNALS_MASK;
}

bool SignalTable::dequeue_next(uint32_t& out_sig) noexcept {
    out_sig = 0U;
    if (m_pending == 0ULL) {
        return false;
    }

    const uint64_t deliverable = m_pending & (~m_blocked | UNMASKABLE_MASK);
    if (deliverable == 0ULL) {
        return false;
    }

    for (uint32_t sig_num = 1U; sig_num <= MAX_SIGNALS; ++sig_num) {
        if ((deliverable & sig_bit(sig_num)) != 0ULL) {
            m_pending &= ~sig_bit(sig_num);
            out_sig = sig_num;
            return true;
        }
    }
    return false;
}

bool SignalTable::has_unblocked_pending() const noexcept {
    const uint64_t deliverable = m_pending & (~m_blocked | UNMASKABLE_MASK);
    if (deliverable == 0ULL) {
        return false;
    }

    for (uint32_t sig = 1U; sig <= MAX_SIGNALS; ++sig) {
        if ((deliverable & sig_bit(sig)) == 0ULL) {
            continue;
        }
        if (sig == SIG_KILL || sig == SIG_STOP) {
            return true;
        }
        const uint64_t handler = m_actions[sig - 1U].handler;
        if (handler == KERNEL_SIG_IGN) {
            continue;
        }
        if (handler == KERNEL_SIG_DFL && (sig == SIG_CHLD || sig == SIG_CONT)) {
            continue;
        }
        return true;
    }
    return false;
}

bool SignalTable::is_deliverable_for_wakeup(uint32_t sig) const noexcept {
    if (!is_valid_signal(sig)) {
        return false;
    }
    const bool is_unmaskable = (sig == SIG_KILL || sig == SIG_STOP);
    if (is_unmaskable) {
        return true;
    }
    const bool is_blocked = (m_blocked & sig_bit(sig)) != 0ULL;
    const uint64_t handler = m_actions[sig - 1U].handler;
    const bool is_ignored = (handler == KERNEL_SIG_IGN) ||
                            (handler == KERNEL_SIG_DFL && (sig == SIG_CHLD || sig == SIG_CONT));
    return !is_blocked && !is_ignored;
}

void SignalTable::block(uint64_t mask) noexcept {
    m_blocked |= (mask & ~UNMASKABLE_MASK);
}

void SignalTable::unblock(uint64_t mask) noexcept {
    m_blocked &= ~(mask & ~UNMASKABLE_MASK);
}

void SignalTable::set_blocked(uint64_t mask) noexcept {
    m_blocked = mask & ~UNMASKABLE_MASK;
}

uint64_t SignalTable::swap_temp_mask(uint64_t temp_mask) noexcept {
    const uint64_t prev_mask = m_blocked;
    m_blocked = temp_mask & ~UNMASKABLE_MASK;
    return prev_mask;
}

void SignalTable::restore_saved_mask_if_any() noexcept {
    if (m_has_saved_mask) {
        m_blocked = m_saved_mask & ~UNMASKABLE_MASK;
        m_has_saved_mask = false;
    }
}

void SignalTable::preserve_saved_mask_for_eintr(uint64_t saved) noexcept {
    m_saved_mask = saved & ~UNMASKABLE_MASK;
    m_has_saved_mask = true;
}

uint64_t SignalTable::consume_saved_or_blocked_mask() noexcept {
    const uint64_t mask = m_has_saved_mask ? m_saved_mask : m_blocked;
    m_has_saved_mask = false;
    return mask;
}

const SigAction& SignalTable::action(uint32_t sig) const noexcept {
    static constexpr SigAction kDefaultAction{};
    if (!is_valid_signal(sig)) {
        return kDefaultAction;
    }
    return m_actions[sig - 1U];
}

bool SignalTable::set_action(uint32_t sig, const SigAction& act) noexcept {
    if (!is_valid_signal(sig) || sig == SIG_KILL || sig == SIG_STOP) {
        return false;
    }
    m_actions[sig - 1U] = act;
    return true;
}

void SignalTable::reset_caught_for_exec() noexcept {
    for (auto& act : m_actions) {
        if (act.handler != KERNEL_SIG_IGN) {
            act = SigAction{};
        }
    }
}

void SignalTable::clone_from(const SignalTable& src) noexcept {
    m_pending = 0ULL;
    m_blocked = src.m_blocked & ~UNMASKABLE_MASK;
    m_saved_mask = 0ULL;
    m_has_saved_mask = false;
    for (size_t idx = 0U; idx < MAX_SIGNALS; ++idx) {
        m_actions[idx] = src.m_actions[idx];
    }
}

void send_signal(Process& target, uint32_t sig) noexcept {
    if (sig == 0U || sig > MAX_SIGNALS || target.state() == ProcessState::Unused ||
        target.state() == ProcessState::Zombie) {
        return;
    }

    const Sync::IrqGuard irq_guard;

    if (sig == SIG_CONT) {
        target.signals().clear_stop_signals();
        if (target.state() == ProcessState::Stopped) {
            target.mark_continued();
            Scheduler::enqueue(&target);
            if (target.ppid() != 0U) {
                auto* const parent = Process::find_by_pid(target.ppid());
                if (parent != nullptr) {
                    parent->child_wait_queue().wake_all();
                }
            }
        }
    }

    target.signals().pend(sig);

    if (target.state() == ProcessState::Blocked &&
        target.signals().is_deliverable_for_wakeup(sig)) {
        if (target.active_wait_queue() != nullptr) {
            target.active_wait_queue()->remove(&target);
        }
        Scheduler::enqueue(&target);
    }
}

void signal_process_group(uint32_t pgid, uint32_t sig) noexcept {
    if (pgid == 0U || sig == 0U || sig > MAX_SIGNALS) {
        return;
    }
    for (Process* proc = Process::list_head(); proc != nullptr; proc = proc->next_global()) {
        if (proc->state() != ProcessState::Unused && proc->state() != ProcessState::Zombie &&
            proc->pgid() == pgid) {
            send_signal(*proc, sig);
        }
    }
}

bool has_unblocked_pending_signal(const Process& proc) noexcept {
    return proc.signals().has_unblocked_pending();
}

void deliver_pending_signals(HAL::CpuContext& ctx) noexcept {
    auto* const proc = Process::current();
    if (proc == nullptr) {
        return;
    }

    uint32_t sig = 0U;
    if (!proc->signals().dequeue_next(sig)) {
        proc->signals().restore_saved_mask_if_any();
        return;
    }

    if (sig == SIG_KILL) {
        exit_current_process(128 + static_cast<int64_t>(sig));
    }

    const SigAction& act = proc->signals().action(sig);
    if (act.handler == KERNEL_SIG_IGN) {
        proc->signals().restore_saved_mask_if_any();
        return;
    }

    if (act.handler == KERNEL_SIG_DFL) {
        if (sig == SIG_CHLD || sig == SIG_CONT) {
            proc->signals().restore_saved_mask_if_any();
            return;
        }
        if (is_stop_signal(sig)) {
            proc->signals().restore_saved_mask_if_any();
            HAL::get_cpu().disable_interrupts();
            proc->mark_stopped(sig);
            if (proc->ppid() != 0U) {
                auto* const parent = Process::find_by_pid(proc->ppid());
                if (parent != nullptr) {
                    send_signal(*parent, SIG_CHLD);
                    parent->child_wait_queue().wake_all();
                }
            }
            Scheduler::schedule();
            return;
        }
        // Default termination action
        exit_current_process(128 + static_cast<int64_t>(sig));
    }

    // Custom EL0 signal handler: build linux_rt_sigframe on user stack
    auto& cpu = HAL::get_cpu();
    const uintptr_t old_sp = cpu.get_user_stack_pointer(ctx);
    if (old_sp <= sizeof(linux_rt_sigframe) + 64U) {
        exit_current_process(128 + static_cast<int64_t>(SIG_KILL));
    }

    const uintptr_t frame_vaddr = (old_sp - sizeof(linux_rt_sigframe)) & ~0xFULL;
    linux_rt_sigframe frame{};
    frame.magic = Kernel::UAPI::RT_SIGFRAME_MAGIC;
    frame.saved_mask = proc->signals().consume_saved_or_blocked_mask();

    for (size_t i = 0U; i < 31U; ++i) {
        frame.regs[i] = cpu.get_reg(ctx, i);
    }
    frame.sp_el0 = old_sp;
    frame.elr_el1 = cpu.get_user_pc(ctx);
    frame.spsr_el1 = cpu.get_user_pstate(ctx);
    frame.tpidr_el0 = cpu.get_user_tls_pointer(ctx);
    frame.trampoline[0] = 0xD2801168U; // mov x8, #139 (SYS_RT_SIGRETURN)
    frame.trampoline[1] = 0xD4000001U; // svc #0

    if (MM::copy_to_user(proc->address_space(), frame_vaddr, &frame, sizeof(frame)) < 0) {
        exit_current_process(128 + static_cast<int64_t>(SIG_KILL));
    }

    // Flush instruction cache if fallback stack trampoline is used
    uintptr_t tramp_paddr = 0U;
    HAL::PageFlags tramp_flags = HAL::PageFlags::None;
    const uintptr_t tramp_vaddr = frame_vaddr + offsetof(linux_rt_sigframe, trampoline);
    if (proc->address_space().query_user_page(tramp_vaddr, tramp_paddr, tramp_flags)) {
        cpu.flush_dcache_range(MM::phys_to_virt(tramp_paddr), sizeof(frame.trampoline));
        cpu.invalidate_icache();
    }

    const uintptr_t restorer_lr = ((act.flags & SA_FLAG_RESTORER) != 0ULL && act.restorer != 0ULL)
                                      ? static_cast<uintptr_t>(act.restorer)
                                      : tramp_vaddr;

    proc->signals().block(act.mask | (1ULL << (sig - 1U)));

    cpu.set_user_stack_pointer(ctx, frame_vaddr);
    cpu.set_user_pc(ctx, static_cast<uintptr_t>(act.handler));
    cpu.set_reg(ctx, 0U, sig);
    cpu.set_reg(ctx, 1U, 0ULL);
    cpu.set_reg(ctx, 2U, frame_vaddr);
    cpu.set_reg(ctx, 30U, restorer_lr);
}

int64_t restore_signal_frame(HAL::CpuContext& ctx) noexcept {
    auto* const proc = Process::current();
    if (proc == nullptr) {
        return -1;
    }

    auto& cpu = HAL::get_cpu();
    const uintptr_t frame_vaddr = cpu.get_user_stack_pointer(ctx);
    linux_rt_sigframe frame{};
    if (MM::copy_from_user(proc->address_space(), &frame, frame_vaddr, sizeof(frame)) < 0) {
        exit_current_process(128 + static_cast<int64_t>(SIG_KILL));
    }

    if (frame.magic != Kernel::UAPI::RT_SIGFRAME_MAGIC) {
        exit_current_process(128 + static_cast<int64_t>(SIG_KILL));
    }

    proc->signals().set_blocked(frame.saved_mask);

    for (size_t i = 0U; i < 31U; ++i) {
        cpu.set_reg(ctx, i, frame.regs[i]);
    }
    cpu.set_user_stack_pointer(ctx, static_cast<uintptr_t>(frame.sp_el0));
    cpu.set_user_pc(ctx, static_cast<uintptr_t>(frame.elr_el1));
    cpu.set_user_pstate(ctx, frame.spsr_el1 & ~0x1FU); // Enforce EL0t mode
    cpu.set_user_tls_pointer(ctx, static_cast<uintptr_t>(frame.tpidr_el0));

    return static_cast<int64_t>(frame.regs[0]);
}

} // namespace Kernel::Process
