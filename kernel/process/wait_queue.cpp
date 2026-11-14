#include "process/wait_queue.hpp"

#include "hal/cpu.hpp"
#include "process/process.hpp"
#include "process/scheduler.hpp"

namespace Kernel::Process {

bool WaitQueue::enqueue(Process* proc) noexcept {
    if (m_count >= MAX_WAITERS || proc == nullptr) {
        return false;
    }
    m_waiters[m_tail] = proc;
    m_tail = (m_tail + 1U) % MAX_WAITERS;
    m_count++;
    return true;
}

Process* WaitQueue::dequeue() noexcept {
    if (m_count == 0U) {
        return nullptr;
    }
    Process* proc = m_waiters[m_head];
    m_waiters[m_head] = nullptr;
    m_head = (m_head + 1U) % MAX_WAITERS;
    m_count--;
    return proc;
}

void WaitQueue::sleep() noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    auto* const current = current_process();
    if (current != nullptr && enqueue(current)) {
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

    while (m_count > 0U) {
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