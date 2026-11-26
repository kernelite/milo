#include "process/scheduler.hpp"

#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "mm/AddressSpace.hpp"
#include "sync/IrqGuard.hpp"

namespace {

constinit Kernel::Scheduler::Runqueue g_runqueue{};
HAL::KernelContext g_idle_kctx{};
uint64_t g_timer_ticks = 0ULL;
bool g_in_schedule = false;

[[noreturn]] void scheduler_atomic_panic(const char* msg) noexcept {
    size_t len = 0U;
    while (msg[len] != '\0') {
        len++;
    }
    HAL::get_console().write(msg, len);
    while (true) {
        HAL::get_cpu().halt();
    }
}

} // namespace

namespace Kernel::Scheduler {

void Runqueue::enqueue(Process::Process* proc) noexcept {
    if (proc == nullptr || proc->m_on_runqueue) {
        return;
    }

    proc->m_rq_prev = m_tail;
    proc->m_rq_next = nullptr;
    if (m_tail != nullptr) {
        m_tail->m_rq_next = proc;
    } else {
        m_head = proc;
    }
    m_tail = proc;
    proc->m_on_runqueue = true;
    m_count++;
}

Process::Process* Runqueue::dequeue() noexcept {
    if (m_head == nullptr) {
        return nullptr;
    }

    Process::Process* const proc = m_head;
    m_head = proc->m_rq_next;
    if (m_head != nullptr) {
        m_head->m_rq_prev = nullptr;
    } else {
        m_tail = nullptr;
    }

    proc->m_rq_next = nullptr;
    proc->m_rq_prev = nullptr;
    proc->m_on_runqueue = false;
    m_count--;
    return proc;
}

void Runqueue::remove(Process::Process* proc) noexcept {
    if (proc == nullptr || !proc->m_on_runqueue) {
        return;
    }

    if (proc->m_rq_prev != nullptr) {
        proc->m_rq_prev->m_rq_next = proc->m_rq_next;
    } else {
        m_head = proc->m_rq_next;
    }

    if (proc->m_rq_next != nullptr) {
        proc->m_rq_next->m_rq_prev = proc->m_rq_prev;
    } else {
        m_tail = proc->m_rq_prev;
    }

    proc->m_rq_next = nullptr;
    proc->m_rq_prev = nullptr;
    proc->m_on_runqueue = false;
    m_count--;
}

void init() noexcept {
    const Sync::IrqGuard irq_guard;
    g_runqueue = Runqueue{};
    g_timer_ticks = 0ULL;
    g_in_schedule = false;
}

void enqueue(Process::Process* proc) noexcept {
    if (proc == nullptr) {
        return;
    }

    const Sync::IrqGuard irq_guard;
    if (proc->state() != Process::ProcessState::Zombie &&
        proc->state() != Process::ProcessState::Unused) {
        if (proc->active_wait_queue() != nullptr) {
            proc->active_wait_queue()->remove(proc);
        }
        proc->set_state(Process::ProcessState::Ready);
        if (proc->timeslice() == 0U) {
            proc->reset_timeslice(DEFAULT_TIMESLICE_TICKS);
        }
        g_runqueue.enqueue(proc);
    }
}

void remove(Process::Process* proc) noexcept {
    if (proc == nullptr) {
        return;
    }

    const Sync::IrqGuard irq_guard;
    g_runqueue.remove(proc);
}

void yield() noexcept {
    const Sync::IrqGuard irq_guard;
    schedule();
}

bool is_in_atomic_context() noexcept {
    return g_in_schedule || !HAL::get_cpu().interrupts_enabled();
}

void schedule() noexcept {
    auto& cpu = HAL::get_cpu();

    // Debug assertion: schedule() must execute strictly inside an IRQ-disabled atomic boundary
    if (cpu.interrupts_enabled()) {
        scheduler_atomic_panic("\r\n[PANIC] Scheduler::schedule() entered with IRQs enabled!\r\n");
    }
    if (g_in_schedule) {
        scheduler_atomic_panic("\r\n[PANIC] Recursive Scheduler::schedule() re-entry!\r\n");
    }

    g_in_schedule = true;

    Process::Process* const prev = Process::Process::current();
    if (prev != nullptr && prev->state() == Process::ProcessState::Running) {
        if (g_runqueue.empty()) {
            prev->reset_timeslice(DEFAULT_TIMESLICE_TICKS);
            g_in_schedule = false;
            return;
        }
        prev->set_state(Process::ProcessState::Ready);
        g_runqueue.enqueue(prev);
    }

    Process::Process* next = g_runqueue.dequeue();
    while (next != nullptr && next->state() != Process::ProcessState::Ready) {
        next = g_runqueue.dequeue();
    }

    if (next == nullptr) {
        Process::Process::set_current(nullptr);
        g_in_schedule = false;
        if (prev != nullptr) {
            HAL::KernelContext* const prev_kctx =
                (prev->state() == Process::ProcessState::Zombie) ? nullptr : &prev->kctx();
            cpu.switch_kernel_context(prev_kctx, &g_idle_kctx);
        }
        return;
    }

    if (!next->is_kctx_initialized()) {
        next->prepare_kstack();
    }

    next->set_state(Process::ProcessState::Running);
    if (next->timeslice() == 0U) {
        next->reset_timeslice(DEFAULT_TIMESLICE_TICKS);
    }
    Process::Process::set_current(next);

    if (next->address_space().is_valid()) {
        next->address_space().activate();
    }

    g_in_schedule = false;

    if (prev == next) {
        return;
    }

    HAL::KernelContext* prev_kctx = nullptr;
    if (prev == nullptr) {
        prev_kctx = &g_idle_kctx;
    } else if (prev->state() != Process::ProcessState::Zombie) {
        prev_kctx = &prev->kctx();
    }

    cpu.switch_kernel_context(prev_kctx, &next->kctx());
}

void on_timer_tick(bool from_user) noexcept {
    const Sync::IrqGuard irq_guard;
    g_timer_ticks++;

    auto* const current = Process::Process::current();
    if (current != nullptr && current->state() == Process::ProcessState::Running) {
        current->record_cpu_tick(from_user);

        if (current->tick_timeslice()) {
            current->reset_timeslice(DEFAULT_TIMESLICE_TICKS);
            schedule();
        }
    }
}

uint64_t timer_ticks() noexcept {
    return g_timer_ticks;
}

void run_until_idle() noexcept {
    auto& cpu = HAL::get_cpu();
    const Sync::IrqGuard irq_guard;

    while (Process::Process::has_runnable()) {
        if (!g_runqueue.empty()) {
            schedule();
        } else {
            if (!g_runqueue.empty()) {
                continue;
            }
            cpu.enable_interrupts();
            cpu.halt();
            cpu.disable_interrupts();
        }
    }
}

} // namespace Kernel::Scheduler
