#include "process/wait_queue.hpp"

#include "hal/cpu.hpp"
#include "process/Process.hpp"
#include "process/scheduler.hpp"
#include "sync/IrqGuard.hpp"

namespace Kernel::Process {

void WaitQueue::enqueue(Process* proc) noexcept {
    if (proc == nullptr || proc->m_active_wq != nullptr) {
        return;
    }

    proc->m_wq_prev = m_tail;
    proc->m_wq_next = nullptr;
    if (m_tail != nullptr) {
        m_tail->m_wq_next = proc;
    } else {
        m_head = proc;
    }
    m_tail = proc;
    proc->m_active_wq = this;
    m_count++;
}

Process* WaitQueue::dequeue() noexcept {
    if (m_head == nullptr) {
        return nullptr;
    }

    Process* const proc = m_head;
    m_head = proc->m_wq_next;
    if (m_head != nullptr) {
        m_head->m_wq_prev = nullptr;
    } else {
        m_tail = nullptr;
    }

    proc->m_wq_next = nullptr;
    proc->m_wq_prev = nullptr;
    proc->m_active_wq = nullptr;
    m_count--;
    return proc;
}

void WaitQueue::remove(Process* proc) noexcept {
    if (proc == nullptr || proc->m_active_wq != this) {
        return;
    }

    if (proc->m_wq_prev != nullptr) {
        proc->m_wq_prev->m_wq_next = proc->m_wq_next;
    } else {
        m_head = proc->m_wq_next;
    }

    if (proc->m_wq_next != nullptr) {
        proc->m_wq_next->m_wq_prev = proc->m_wq_prev;
    } else {
        m_tail = proc->m_wq_prev;
    }

    proc->m_wq_next = nullptr;
    proc->m_wq_prev = nullptr;
    proc->m_active_wq = nullptr;
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
