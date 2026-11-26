#pragma once

#include "hal/cpu.hpp"

#ifdef HOST_TEST
#include <atomic>
#endif

namespace Kernel::Sync {

// Zero-overhead RAII interrupt-masking guard.
// Captures prior IRQ enable state on construction, masks IRQs,
// and restores prior state on destruction.
class IrqGuard final {
  public:
    IrqGuard() noexcept : m_irq_was_enabled(HAL::get_cpu().interrupts_enabled()) {
        HAL::get_cpu().disable_interrupts();
#ifdef HOST_TEST
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

    ~IrqGuard() noexcept {
#ifdef HOST_TEST
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
        if (m_irq_was_enabled) {
            HAL::get_cpu().enable_interrupts();
        }
    }

    IrqGuard(const IrqGuard&) = delete;
    IrqGuard& operator=(const IrqGuard&) = delete;
    IrqGuard(IrqGuard&&) = delete;
    IrqGuard& operator=(IrqGuard&&) = delete;

  private:
    bool m_irq_was_enabled{false};
};

} // namespace Kernel::Sync
