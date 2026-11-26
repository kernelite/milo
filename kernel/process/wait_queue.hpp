#pragma once

#include <cstddef>

namespace Kernel::Process {

class Process;

class WaitQueue {
  public:
    constexpr WaitQueue() = default;

    void sleep() noexcept;
    void wake_one() noexcept;
    void wake_all() noexcept;
    void remove(Process* proc) noexcept;

    [[nodiscard]] bool empty() const noexcept { return m_head == nullptr; }
    [[nodiscard]] size_t size() const noexcept { return m_count; }

  private:
    Process* m_head{nullptr};
    Process* m_tail{nullptr};
    size_t m_count{0U};

    void enqueue(Process* proc) noexcept;
    Process* dequeue() noexcept;
};

} // namespace Kernel::Process