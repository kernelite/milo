#pragma once

#include "process/process.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Scheduler {

constexpr uint32_t DEFAULT_TIMESLICE_TICKS = 5U; // 50 ms at 100 Hz

class Runqueue {
  public:
    constexpr Runqueue() = default;

    void enqueue(Process::Process* proc) noexcept;
    Process::Process* dequeue() noexcept;
    void remove(Process::Process* proc) noexcept;

    [[nodiscard]] bool empty() const noexcept { return m_head == nullptr; }
    [[nodiscard]] size_t size() const noexcept { return m_count; }

  private:
    Process::Process* m_head{nullptr};
    Process::Process* m_tail{nullptr};
    size_t m_count{0U};
};

void init() noexcept;
void enqueue(Process::Process* proc) noexcept;
void remove(Process::Process* proc) noexcept;
void yield() noexcept;
void schedule() noexcept;
void on_timer_tick(bool from_user = true) noexcept;
uint64_t timer_ticks() noexcept;
[[nodiscard]] bool is_in_atomic_context() noexcept;

// Runs all enqueued processes until none remain Ready/Running/Blocked, then returns
void run_until_idle() noexcept;

} // namespace Kernel::Scheduler