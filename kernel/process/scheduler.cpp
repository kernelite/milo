#include "process/scheduler.hpp"

#include "hal/cpu.hpp"

namespace {

constexpr size_t MAX_RUNQUEUE_SIZE = Kernel::Process::MAX_PROCESSES;

struct Runqueue {
    Kernel::Process::Process* queue[MAX_RUNQUEUE_SIZE]{nullptr};
    size_t head{0U};
    size_t tail{0U};
    size_t count{0U};
};

Runqueue g_rq{};
HAL::KernelContext g_bootstrap_kctx{};
bool g_bootstrap_active = false;
volatile uint64_t g_timer_ticks = 0ULL;

} // namespace

namespace Kernel::Scheduler {

void init() noexcept {
    g_rq.head = 0U;
    g_rq.tail = 0U;
    g_rq.count = 0U;
    g_bootstrap_active = false;
    g_timer_ticks = 0ULL;
}

uint64_t timer_ticks() noexcept {
    return g_timer_ticks;
}

void enqueue(Process::Process* proc) noexcept {
    if (proc == nullptr || proc->state == Process::ProcessState::Unused ||
        proc->state == Process::ProcessState::Zombie) {
        return;
    }

    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    for (size_t i = 0U; i < g_rq.count; ++i) {
        const size_t idx = (g_rq.head + i) % MAX_RUNQUEUE_SIZE;
        if (g_rq.queue[idx] == proc) {
            proc->state = Process::ProcessState::Ready;
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return;
        }
    }

    if (g_rq.count < MAX_RUNQUEUE_SIZE) {
        if (!proc->kctx_initialized) {
            Process::prepare_process_kstack(*proc);
        }
        proc->state = Process::ProcessState::Ready;
        if (proc->timeslice == 0U) {
            proc->timeslice = DEFAULT_TIMESLICE_TICKS;
        }
        g_rq.queue[g_rq.tail] = proc;
        g_rq.tail = (g_rq.tail + 1U) % MAX_RUNQUEUE_SIZE;
        g_rq.count++;
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void yield() noexcept {
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    auto* const current = Process::current_process();
    if (current != nullptr && current->state == Process::ProcessState::Running) {
        current->timeslice = DEFAULT_TIMESLICE_TICKS;
        enqueue(current);
    }
    schedule();

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void schedule() noexcept {
    auto& cpu = HAL::get_cpu();
    cpu.disable_interrupts();

    auto* const current = Process::current_process();

    while (g_rq.count == 0U) {
        if (current != nullptr && current->state == Process::ProcessState::Running) {
            return; // Current process is the sole runnable task
        }

        if (!Process::has_runnable_processes()) {
            // All scheduled processes have exited; return to bootstrap caller
            Process::set_current_process(nullptr);
            if (g_bootstrap_active) {
                g_bootstrap_active = false;
                HAL::KernelContext* prev_kctx = (current != nullptr) ? &current->kctx : nullptr;
                cpu.switch_kernel_context(prev_kctx, &g_bootstrap_kctx);
            }
            return;
        }

        // At least one process is Blocked waiting for a timer/device IRQ; idle with IRQs enabled
        cpu.enable_interrupts();
        cpu.halt();
        cpu.disable_interrupts();
    }

    Process::Process* const next = g_rq.queue[g_rq.head];
    g_rq.queue[g_rq.head] = nullptr;
    g_rq.head = (g_rq.head + 1U) % MAX_RUNQUEUE_SIZE;
    g_rq.count--;

    if (next == nullptr) {
        return;
    }

    if (next->timeslice == 0U) {
        next->timeslice = DEFAULT_TIMESLICE_TICKS;
    }
    next->state = Process::ProcessState::Running;
    Process::set_current_process(next);

    if (current == next) {
        return;
    }

    if (next->address_space.is_valid()) {
        next->address_space.activate();
    }

    HAL::KernelContext* const prev_kctx =
        (current != nullptr) ? &current->kctx : (g_bootstrap_active ? &g_bootstrap_kctx : nullptr);
    cpu.switch_kernel_context(prev_kctx, &next->kctx);
}

void on_timer_tick() noexcept {
    g_timer_ticks = g_timer_ticks + 1ULL;

    auto* const current = Process::current_process();
    if (current == nullptr || current->state != Process::ProcessState::Running) {
        return;
    }

    current->cpu_ticks++;

    if (current->timeslice > 0U) {
        current->timeslice--;
    }

    if (current->timeslice == 0U) {
        current->timeslice = DEFAULT_TIMESLICE_TICKS;
        enqueue(current);
        schedule();
    }
}

void run_until_idle() noexcept {
    auto& cpu = HAL::get_cpu();
    cpu.disable_interrupts();
    g_bootstrap_active = true;
    schedule();
    cpu.disable_interrupts();
}

} // namespace Kernel::Scheduler