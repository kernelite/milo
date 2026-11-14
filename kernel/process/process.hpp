#pragma once

#include "hal/cpu.hpp"
#include "loader/elf_loader.hpp"
#include "mm/address_space.hpp"
#include "process/stack_builder.hpp"
#include "process/wait_queue.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Process {

constexpr size_t MAX_PROCESSES = 16U;
constexpr size_t MAX_FDS_PER_PROCESS = 16U;
constexpr size_t KERNEL_STACK_PAGES = 4U; // 16 KiB per-process kernel stack

constexpr uint32_t FD_FLAG_READ = 1U << 0U;
constexpr uint32_t FD_FLAG_WRITE = 1U << 1U;

enum class ProcessState : uint8_t { Unused = 0, Ready, Running, Blocked, Zombie };

enum class FdType : uint8_t { None = 0, ConsoleTty, InitrdFile };

struct FileDescriptor {
    FdType type{FdType::None};
    uint32_t flags{0U};
    size_t offset{0U};
    bool in_use{false};

    [[nodiscard]] constexpr bool is_readable() const noexcept {
        return in_use && ((flags & FD_FLAG_READ) != 0U);
    }

    [[nodiscard]] constexpr bool is_writable() const noexcept {
        return in_use && ((flags & FD_FLAG_WRITE) != 0U);
    }

    [[nodiscard]] constexpr bool is_tty() const noexcept {
        return in_use && (type == FdType::ConsoleTty);
    }
};

struct Process {
    uint32_t pid{0U};
    uint32_t ppid{0U};
    ProcessState state{ProcessState::Unused};
    int64_t exit_status{0};
    uint32_t timeslice{0U};
    uint64_t cpu_ticks{0U};
    MM::AddressSpace address_space{};
    HAL::CpuContext context{};
    HAL::KernelContext kctx{};
    uintptr_t kstack_paddr{0U};
    uintptr_t kstack_top{0U};
    bool kctx_initialized{false};
    UserStack user_stack{};
    uintptr_t entry_point{0U};
    uintptr_t brk_start{0U};
    uintptr_t brk_current{0U};
    uintptr_t mmap_next_vaddr{0U};
    uintptr_t clear_child_tid{0U};
    WaitQueue child_wait_queue{};
    FileDescriptor fd_table[MAX_FDS_PER_PROCESS]{};
};

uint64_t last_cpu_ticks_for_pid(uint32_t pid) noexcept;

struct ProcessContext {
    HAL::CpuContext cpu_ctx{};
    UserStack user_stack{};
    uintptr_t entry_point{0U};
};

// Static process table management (zero dynamic heap allocation)
Process* allocate_process(uint32_t ppid = 0U) noexcept;
void release_process(Process& proc) noexcept;
Process* find_process_by_pid(uint32_t pid) noexcept;
Process* process_at(size_t index) noexcept;
size_t active_process_count() noexcept;
bool has_runnable_processes() noexcept;

Process* current_process() noexcept;
void set_current_process(Process* proc) noexcept;

void init_user_cpu_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t sp_el0,
                           uint64_t argc, uintptr_t argv_vaddr) noexcept;

// Initializes the process's EL1 kernel stack frame from proc.context prior to scheduling
void prepare_process_kstack(Process& proc) noexcept;

inline int64_t enter_user_context(const HAL::CpuContext* ctx) noexcept {
    if (ctx == nullptr) {
        return -1;
    }
    return HAL::get_cpu().enter_user_context(*ctx);
}

// Schedules the process on the preemptive runqueue until it and its descendants exit
int64_t execute_process(Process& proc) noexcept;

// Marks the current process Zombie, wakes parent wait queue, and yields the CPU permanently
[[noreturn]] void exit_current_process(int64_t exit_code) noexcept;

void attach_loaded_elf(Process& proc, Loader::LoadedElf&& loaded) noexcept;

} // namespace Kernel::Process