#include "fs/pipe.hpp"

#include "hal/cpu.hpp"
#include "mm/address_space.hpp"
#include "process/process.hpp"

#include <new>

namespace {

constexpr int64_t ERR_EINTR = -4;
constexpr int64_t ERR_EAGAIN = -11;
constexpr int64_t ERR_EFAULT = -14;
constexpr int64_t ERR_EINVAL = -22;
constexpr int64_t ERR_EPIPE = -32;

} // namespace

namespace Kernel::FS {

Pipe* allocate_pipe() noexcept {
    auto* const pipe = new (std::nothrow) Pipe{};
    if (pipe == nullptr) {
        return nullptr;
    }

    pipe->in_use = true;
    pipe->readers = 1U;
    pipe->writers = 1U;
    pipe->read_pos = 0U;
    pipe->write_pos = 0U;
    pipe->count = 0U;
    return pipe;
}

void retain_pipe_reader(Pipe* pipe) noexcept {
    if (pipe == nullptr) {
        return;
    }
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    if (pipe->in_use) {
        pipe->readers++;
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void retain_pipe_writer(Pipe* pipe) noexcept {
    if (pipe == nullptr) {
        return;
    }
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    if (pipe->in_use) {
        pipe->writers++;
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
}

void release_pipe_reader(Pipe* pipe) noexcept {
    if (pipe == nullptr) {
        return;
    }
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    if (!pipe->in_use) {
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
        return;
    }

    if (pipe->readers > 0U) {
        pipe->readers--;
    }
    pipe->write_wq.wake_all();

    const bool should_delete = (pipe->readers == 0U && pipe->writers == 0U);
    if (should_delete) {
        pipe->in_use = false;
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }

    if (should_delete) {
        delete pipe;
    }
}

void release_pipe_writer(Pipe* pipe) noexcept {
    if (pipe == nullptr) {
        return;
    }
    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    if (!pipe->in_use) {
        if (irq_was_enabled) {
            cpu.enable_interrupts();
        }
        return;
    }

    if (pipe->writers > 0U) {
        pipe->writers--;
    }
    pipe->read_wq.wake_all();

    const bool should_delete = (pipe->readers == 0U && pipe->writers == 0U);
    if (should_delete) {
        pipe->in_use = false;
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }

    if (should_delete) {
        delete pipe;
    }
}

int64_t pipe_read(Pipe* pipe, uintptr_t root_paddr, uintptr_t user_buf, size_t count,
                  bool nonblock) noexcept {
    if (pipe == nullptr || !pipe->in_use) {
        return ERR_EINVAL;
    }
    if (count == 0U) {
        return 0;
    }

    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    while (pipe->count == 0U) {
        if (pipe->writers == 0U) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return 0; // EOF
        }
        if (nonblock) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return ERR_EAGAIN;
        }
        const auto* const cur = Process::current_process();
        if (cur != nullptr && Process::has_unblocked_pending_signal(*cur)) {
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return ERR_EINTR;
        }
        pipe->read_wq.sleep();
        cpu.disable_interrupts();
    }

    const size_t to_read = (count < pipe->count) ? count : pipe->count;
    uint8_t staging[256]{};
    size_t copied = 0U;

    while (copied < to_read) {
        const size_t batch =
            ((to_read - copied) < sizeof(staging)) ? (to_read - copied) : sizeof(staging);
        for (size_t i = 0U; i < batch; ++i) {
            staging[i] = pipe->buffer[pipe->read_pos];
            pipe->read_pos = (pipe->read_pos + 1U) % PIPE_BUF_SIZE;
        }
        pipe->count -= batch;

        const int64_t res = MM::copy_to_user(root_paddr, user_buf + copied, staging, batch);
        if (res < 0) {
            pipe->write_wq.wake_all();
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return (copied > 0U) ? static_cast<int64_t>(copied) : ERR_EFAULT;
        }
        copied += batch;
    }

    pipe->write_wq.wake_all();
    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
    return static_cast<int64_t>(copied);
}

int64_t pipe_write(Pipe* pipe, uintptr_t root_paddr, uintptr_t user_buf, size_t count,
                   bool nonblock) noexcept {
    if (pipe == nullptr || !pipe->in_use) {
        return ERR_EINVAL;
    }
    if (count == 0U) {
        return 0;
    }

    auto& cpu = HAL::get_cpu();
    const bool irq_was_enabled = cpu.interrupts_enabled();
    cpu.disable_interrupts();

    size_t total_written = 0U;
    uint8_t staging[256]{};

    while (total_written < count) {
        if (pipe->readers == 0U) {
            pipe->read_wq.wake_all();
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : ERR_EPIPE;
        }

        while (pipe->count == PIPE_BUF_SIZE) {
            if (pipe->readers == 0U) {
                if (irq_was_enabled) {
                    cpu.enable_interrupts();
                }
                return (total_written > 0U) ? static_cast<int64_t>(total_written) : ERR_EPIPE;
            }
            if (nonblock) {
                pipe->read_wq.wake_all();
                if (irq_was_enabled) {
                    cpu.enable_interrupts();
                }
                return (total_written > 0U) ? static_cast<int64_t>(total_written) : ERR_EAGAIN;
            }
            const auto* const cur = Process::current_process();
            if (cur != nullptr && Process::has_unblocked_pending_signal(*cur)) {
                if (irq_was_enabled) {
                    cpu.enable_interrupts();
                }
                return (total_written > 0U) ? static_cast<int64_t>(total_written) : ERR_EINTR;
            }
            pipe->write_wq.sleep();
            cpu.disable_interrupts();
        }

        const size_t space = PIPE_BUF_SIZE - pipe->count;
        const size_t rem = count - total_written;
        size_t batch = (rem < space) ? rem : space;
        if (batch > sizeof(staging)) {
            batch = sizeof(staging);
        }

        const int64_t res =
            MM::copy_from_user(root_paddr, staging, user_buf + total_written, batch);
        if (res < 0) {
            pipe->read_wq.wake_all();
            if (irq_was_enabled) {
                cpu.enable_interrupts();
            }
            return (total_written > 0U) ? static_cast<int64_t>(total_written) : ERR_EFAULT;
        }

        for (size_t i = 0U; i < batch; ++i) {
            pipe->buffer[pipe->write_pos] = staging[i];
            pipe->write_pos = (pipe->write_pos + 1U) % PIPE_BUF_SIZE;
        }
        pipe->count += batch;
        total_written += batch;
        pipe->read_wq.wake_all();
    }

    if (irq_was_enabled) {
        cpu.enable_interrupts();
    }
    return static_cast<int64_t>(total_written);
}

} // namespace Kernel::FS