#include "process/wait_queue.hpp"

#include "hal/cpu.hpp"
#include "process/Process.hpp"
#include "process/scheduler.hpp"
#include "sync/IrqGuard.hpp"

namespace Kernel::Process {

void WaitQueue::enqueue(Process* proc) noexcept {
    if (proc == nullptr || proc->active_wait_queue() != nullptr) {
        return;
    }

    proc->set_wq_links(m_tail, nullptr);
    if (m_tail != nullptr) {
        m_tail->set_wq_links(m_tail->wq_prev(), proc);
    } else {
        m_head = proc;
    }
    m_tail = proc;
    proc->set_active_wait_queue(this);
    m_count++;
}

Process* WaitQueue::dequeue() noexcept {
    if (m_head == nullptr) {
        return nullptr;
    }

    Process* const proc = m_head;
    m_head = proc->wq_next();
    if (m_head != nullptr) {
        m_head->set_wq_links(nullptr, m_head->wq_next());
    } else {
        m_tail = nullptr;
    }

    proc->clear_wq_links();
    m_count--;
    return proc;
}

void WaitQueue::remove(Process* proc) noexcept {
    if (proc == nullptr || proc->active_wait_queue() != this) {
        return;
    }

    Process* const prev_proc = proc->wq_prev();
    Process* const next_proc = proc->wq_next();

    if (prev_proc != nullptr) {
        prev_proc->set_wq_links(prev_proc->wq_prev(), next_proc);
    } else {
        m_head = next_proc;
    }

    if (next_proc != nullptr) {
        next_proc->set_wq_links(prev_proc, next_proc->wq_next());
    } else {
        m_tail = prev_proc;
    }

    proc->clear_wq_links();
    m_count--;
}

void WaitQueue::sleep() noexcept {
    const Sync::IrqGuard irq_guard;
    auto* const current = Process::current();
    if (current != nullptr) {
        enqueue(current);
        current->set_state(ProcessState::Blocked);
        Scheduler::schedule();
    }
}

void WaitQueue::wake_one() noexcept {
    const Sync::IrqGuard irq_guard;
    Process* const proc = dequeue();
    if (proc != nullptr) {
        Scheduler::enqueue(proc);
    }
}

void WaitQueue::wake_all() noexcept {
    const Sync::IrqGuard irq_guard;
    while (m_head != nullptr) {
        Process* const proc = dequeue();
        if (proc != nullptr) {
            Scheduler::enqueue(proc);
        }
    }
}

} // namespace Kernel::Process
