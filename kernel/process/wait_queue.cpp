#include "process/wait_queue.hpp"

#include "hal/cpu.hpp"
#include "process/Process.hpp"
#include "process/scheduler.hpp"

namespace Kernel::Process {

void WaitQueue::enqueue(Process* proc) noexcept {
    if (proc == nullptr || proc->active_wq != nullptr) {
        return;
    }

    proc->wq_prev = m_tail;
    proc->wq_next = nullptr;
    if (m_tail != nullptr) {
        m_tail->wq_next = proc;
    } else {
        m_head = proc;
    }
    m_tail = proc;
    proc->active_wq = this;
    m_count++;
}

Process* WaitQueue::dequeue() noexcept {
    if (m_head == nullptr) {
        return nullptr;
    }

    Process* const proc = m_head;
    m_head = proc->wq_next;
    if (m_head != nullptr) {
        m_head->wq_prev = nullptr;
    } else {
        m_tail = nullptr;
    }

    proc->wq_next = nullptr;
    proc->wq_prev = nullptr;
    proc->active_wq = nullptr;
    m_count--;
    return proc;
}

void WaitQueue::remove(Process* proc) noexcept {
    if (proc == nullptr || proc->active_wq != this) {
        return;
    }

    if (proc->wq_prev != nullptr) {
        proc->wq_prev->wq_next = proc->wq_next;
    } else {
        m_head = proc->wq_next;
    }

    if (proc->wq_next != nullptr) {
        proc->wq_next->wq_prev = proc->wq_prev;
    } else {
        m_tail = proc->wq_prev;
    }

    proc->wq_next = nullptr;
    proc->wq_prev = nullptr;
    proc->active_wq = nullptr;
    m_count--;
}

void WaitQueue::sleep() noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    auto* const current = current_process();
    if (current != nullptr) {
        enqueue(current);
        current->state = ProcessState::Blocked;
        Scheduler::schedule();
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void WaitQueue::wake_one() noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    Process* const proc = dequeue();
    if (proc != nullptr) {
        Scheduler::enqueue(proc);
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void WaitQueue::wake_all() noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    while (m_head != nullptr) {
        Process* const proc = dequeue();
        if (proc != nullptr) {
            Scheduler::enqueue(proc);
        }
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

} // namespace Kernel::Process