#include "hal/targets/qemu_virt/timer.hpp"

namespace {
uint64_t g_cntfrq = 0ULL;
uint32_t g_reload_val = 0U;
} // namespace

namespace HAL::QemuVirt {

void GenericTimer::init(uint32_t freq_hz) noexcept {
    asm volatile("mrs %0, cntfrq_el0" : "=r"(g_cntfrq));
    if (freq_hz == 0U) {
        freq_hz = 100U;
    }
    g_reload_val = static_cast<uint32_t>(g_cntfrq / freq_hz);
    if (g_reload_val == 0U) {
        g_reload_val = 1U;
    }
    reload();
    // Enable EL1 physical timer (ENABLE=1, IMASK=0)
    asm volatile("msr cntp_ctl_el0, %0\n\tisb" ::"r"(1ULL) : "memory");
}

void GenericTimer::disable() noexcept {
    // Disable EL1 physical timer and mask output (ENABLE=0, IMASK=1)
    asm volatile("msr cntp_ctl_el0, %0\n\tisb" ::"r"(2ULL) : "memory");
}

void GenericTimer::reload() noexcept {
    asm volatile("msr cntp_tval_el0, %0\n\tisb" ::"r"(static_cast<uint64_t>(g_reload_val))
                 : "memory");
}

uint64_t GenericTimer::frequency_hz() noexcept {
    return g_cntfrq;
}

uint64_t GenericTimer::counter_ticks() noexcept {
    uint64_t ticks = 0ULL;
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks;
}

} // namespace HAL::QemuVirt