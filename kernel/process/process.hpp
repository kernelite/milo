#pragma once

#include "hal/cpu.hpp"
#include "loader/elf_loader.hpp"
#include "mm/address_space.hpp"
#include "process/stack_builder.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Process {

constexpr size_t MAX_PROCESSES = 16U;
constexpr size_t MAX_FDS_PER_PROCESS = 16U;

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
    MM::AddressSpace address_space{};
    HAL::CpuContext context{};
    UserStack user_stack{};
    uintptr_t entry_point{0U};
    uintptr_t brk_start{0U};
    uintptr_t brk_current{0U};
    uintptr_t mmap_next_vaddr{0U};
    FileDescriptor fd_table[MAX_FDS_PER_PROCESS]{};
};

struct ProcessContext {
    HAL::CpuContext cpu_ctx{};
    UserStack user_stack{};
    uintptr_t entry_point{0U};
};

// Static process table management (zero dynamic heap allocation)
Process* allocate_process(uint32_t ppid = 0U) noexcept;
void release_process(Process& proc) noexcept;
Process* find_process_by_pid(uint32_t pid) noexcept;
size_t active_process_count() noexcept;

Process* current_process() noexcept;
void set_current_process(Process* proc) noexcept;

void init_user_cpu_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t sp_el0,
                           uint64_t argc, uintptr_t argv_vaddr) noexcept;

inline int64_t enter_user_context(const HAL::CpuContext* ctx) noexcept {
    if (ctx == nullptr) {
        return -1;
    }
    return HAL::get_cpu().enter_user_context(*ctx);
}

// Activates the process's AddressSpace, enters user mode via HAL, and restores state on return
int64_t execute_process(Process& proc) noexcept;

void attach_loaded_elf(Process& proc, Loader::LoadedElf&& loaded) noexcept;

} // namespace Kernel::Process