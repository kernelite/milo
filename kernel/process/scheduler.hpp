#pragma once

#include "process/process.hpp"

#include <cstdint>

namespace Kernel::Scheduler {

constexpr uint32_t DEFAULT_TIMESLICE_TICKS = 5U; // 50 ms at 100 Hz

void init() noexcept;
void enqueue(Process::Process* proc) noexcept;
void yield() noexcept;
void schedule() noexcept;
void on_timer_tick() noexcept;
uint64_t timer_ticks() noexcept;

// Runs all enqueued processes until none remain Ready/Running/Blocked, then returns
void run_until_idle() noexcept;

} // namespace Kernel::Scheduler