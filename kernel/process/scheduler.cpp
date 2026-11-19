#include "process/scheduler.hpp"

#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "mm/address_space.hpp"

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
    if (proc == nullptr || proc->on_runqueue) {
        return;
    }

    proc->rq_prev = m_tail;
    proc->rq_next = nullptr;
    if (m_tail != nullptr) {
        m_tail->rq_next = proc;
    } else {
        m_head = proc;
    }
    m_tail = proc;
    proc->on_runqueue = true;
    m_count++;
}

Process::Process* Runqueue::dequeue() noexcept {
    if (m_head == nullptr) {
        return nullptr;
    }

    Process::Process* const proc = m_head;
    m_head = proc->rq_next;
    if (m_head != nullptr) {
        m_head->rq_prev = nullptr;
    } else {
        m_tail = nullptr;
    }

    proc->rq_next = nullptr;
    proc->rq_prev = nullptr;
    proc->on_runqueue = false;
    m_count--;
    return proc;
}

void Runqueue::remove(Process::Process* proc) noexcept {
    if (proc == nullptr || !proc->on_runqueue) {
        return;
    }

    if (proc->rq_prev != nullptr) {
        proc->rq_prev->rq_next = proc->rq_next;
    } else {
        m_head = proc->rq_next;
    }

    if (proc->rq_next != nullptr) {
        proc->rq_next->rq_prev = proc->rq_prev;
    } else {
        m_tail = proc->rq_prev;
    }

    proc->rq_next = nullptr;
    proc->rq_prev = nullptr;
    proc->on_runqueue = false;
    m_count--;
}

void init() noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    g_runqueue = Runqueue{};
    g_timer_ticks = 0ULL;
    g_in_schedule = false;

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void enqueue(Process::Process* proc) noexcept {
    if (proc == nullptr) {
        return;
    }

    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    if (proc->state != Process::ProcessState::Zombie &&
        proc->state != Process::ProcessState::Unused) {
        if (proc->active_wq != nullptr) {
            proc->active_wq->remove(proc);
        }
        proc->state = Process::ProcessState::Ready;
        if (proc->timeslice == 0U) {
            proc->timeslice = DEFAULT_TIMESLICE_TICKS;
        }
        g_runqueue.enqueue(proc);
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void remove(Process::Process* proc) noexcept {
    if (proc == nullptr) {
        return;
    }

    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    g_runqueue.remove(proc);

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void yield() noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    schedule();

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
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

    Process::Process* const prev = Process::current_process();
    if (prev != nullptr && prev->state == Process::ProcessState::Running) {
        if (g_runqueue.empty()) {
            prev->timeslice = DEFAULT_TIMESLICE_TICKS;
            g_in_schedule = false;
            return;
        }
        prev->state = Process::ProcessState::Ready;
        g_runqueue.enqueue(prev);
    }

    Process::Process* next = g_runqueue.dequeue();
    while (next != nullptr && next->state != Process::ProcessState::Ready) {
        next = g_runqueue.dequeue();
    }

    if (next == nullptr) {
        Process::set_current_process(nullptr);
        g_in_schedule = false;
        if (prev != nullptr) {
            HAL::KernelContext* const prev_kctx =
                (prev->state == Process::ProcessState::Zombie) ? nullptr : &prev->kctx;
            cpu.switch_kernel_context(prev_kctx, &g_idle_kctx);
        }
        return;
    }

    if (!next->kctx_initialized) {
        Process::prepare_process_kstack(*next);
    }

    next->state = Process::ProcessState::Running;
    if (next->timeslice == 0U) {
        next->timeslice = DEFAULT_TIMESLICE_TICKS;
    }
    Process::set_current_process(next);

    if (next->address_space.is_valid()) {
        next->address_space.activate();
    }

    g_in_schedule = false;

    if (prev == next) {
        return;
    }

    HAL::KernelContext* prev_kctx = nullptr;
    if (prev == nullptr) {
        prev_kctx = &g_idle_kctx;
    } else if (prev->state != Process::ProcessState::Zombie) {
        prev_kctx = &prev->kctx;
    }

    cpu.switch_kernel_context(prev_kctx, &next->kctx);
}

void on_timer_tick(bool from_user) noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    g_timer_ticks++;

    auto* const current = Process::current_process();
    if (current != nullptr && current->state == Process::ProcessState::Running) {
        if (from_user) {
            current->utime++;
        } else {
            current->stime++;
        }

        if (current->timeslice > 0U) {
            current->timeslice--;
        }
        if (current->timeslice == 0U) {
            current->timeslice = DEFAULT_TIMESLICE_TICKS;
            schedule();
        }
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

uint64_t timer_ticks() noexcept {
    return g_timer_ticks;
}

void run_until_idle() noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    while (Process::has_runnable_processes()) {
        if (!g_runqueue.empty()) {
            schedule();
        } else {
            Process::poll_console_tty();
            if (!g_runqueue.empty()) {
                continue;
            }
            cpu.enable_interrupts();
            cpu.halt();
            cpu.disable_interrupts();
        }
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

} // namespace Kernel::Scheduler