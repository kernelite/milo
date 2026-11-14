#pragma once

#include <cstddef>

namespace Kernel::Process {

struct Process;

constexpr size_t MAX_WAITERS = 16U;

class WaitQueue {
  public:
    constexpr WaitQueue() = default;

    void sleep() noexcept;
    void wake_one() noexcept;
    void wake_all() noexcept;
    [[nodiscard]] bool empty() const noexcept { return m_count == 0U; }

  private:
    Process* m_waiters[MAX_WAITERS]{nullptr};
    size_t m_head{0U};
    size_t m_tail{0U};
    size_t m_count{0U};

    bool enqueue(Process* proc) noexcept;
    Process* dequeue() noexcept;
};

} // namespace Kernel::Process