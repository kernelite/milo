#pragma once

#include "hal/cpu.hpp"
#include "ipc/pipe.hpp"
#include "loader/elf_loader.hpp"
#include "mm/AddressSpace.hpp"
#include "process/FileDescriptor.hpp"
#include "process/Signal.hpp"
#include "process/resource.hpp"
#include "process/stack_builder.hpp"
#include "process/wait_queue.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Process {

constexpr size_t KERNEL_STACK_PAGES = 4U; // 16 KiB per-process kernel stack
constexpr size_t MAX_PATH_LEN = 256U;

enum class ProcessState : uint8_t { Unused = 0, Ready, Running, Blocked, Stopped, Zombie };

struct Process {
    Process() = default;
    ~Process() noexcept;

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) = delete;
    Process& operator=(Process&&) = delete;

    HAL::KernelContext kctx{};
    HAL::CpuContext context{};
    int64_t exit_status{0};

    // Integrated CPU accounting counters (in timer ticks)
    size_t utime{0U};
    size_t stime{0U};
    size_t cutime{0U};
    size_t cstime{0U};

    uint64_t rlim_nofile_cur{DEFAULT_RLIMIT_NOFILE_CUR};
    uint64_t rlim_nofile_max{DEFAULT_RLIMIT_NOFILE_MAX};
    uint64_t rlim_nproc_cur{0ULL};
    uint64_t rlim_nproc_max{0ULL};
    uint64_t saved_sigmask{0ULL};
    MM::AddressSpace address_space;
    uintptr_t kstack_paddr{0U};
    uintptr_t kstack_top{0U};
    uintptr_t entry_point{0U};
    uintptr_t brk_start{0U};
    uintptr_t brk_current{0U};
    uintptr_t mmap_next_vaddr{0U};
    uintptr_t clear_child_tid{0U};
    uint64_t pending_signals{0ULL};
    uint64_t blocked_signals{0ULL};
    UserStack user_stack{};
    WaitQueue child_wait_queue;
    SigAction sigactions[MAX_SIGNALS]{};
    FileDescriptor* fd_table{nullptr};
    size_t fd_capacity{0U};
    uint32_t pid{0U};
    uint32_t ppid{0U};
    uint32_t pgid{0U};
    uint32_t sid{0U};
    uint32_t umask{0022U};
    uint32_t timeslice{0U};
    uint32_t stop_signal{0U};
    ProcessState state{ProcessState::Unused};
    bool stop_reported{false};
    bool continued{false};
    bool has_saved_sigmask{false};
    bool kctx_initialized{false};
    char cwd[MAX_PATH_LEN]{};
    char exe_path[MAX_PATH_LEN]{};

    // Intrusive global process list links
    Process* global_next{nullptr};
    Process* global_prev{nullptr};

    // Intrusive CPU runqueue links
    Process* rq_next{nullptr};
    Process* rq_prev{nullptr};
    bool on_runqueue{false};

    // Intrusive waitqueue links
    Process* wq_next{nullptr};
    Process* wq_prev{nullptr};
    WaitQueue* active_wq{nullptr};
};

uint64_t last_cpu_ticks_for_pid(uint32_t pid) noexcept;

// Heap-allocated dynamic process tracking via intrusive global list
Process* allocate_process(uint32_t ppid = 0U) noexcept;
void release_process(Process& proc) noexcept;
Process* find_process_by_pid(uint32_t pid) noexcept;
Process* process_list_head() noexcept;
size_t active_process_count() noexcept;
bool has_runnable_processes() noexcept;

Process* current_process() noexcept;
void set_current_process(Process* proc) noexcept;

// Console TTY polling & foreground job-control group helpers
void poll_console_tty() noexcept;
void set_foreground_pgid(uint32_t pgid) noexcept;
[[nodiscard]] uint32_t get_foreground_pgid() noexcept;

void init_user_cpu_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t sp_el0,
                           uint64_t argc, uintptr_t argv_vaddr) noexcept;
void prepare_process_kstack(Process& proc) noexcept;

inline int64_t enter_user_context(const HAL::CpuContext* ctx) noexcept {
    if (ctx == nullptr) {
        return -1;
    }
    return HAL::get_cpu().enter_user_context(*ctx);
}

int64_t execute_process(Process& proc) noexcept;
[[noreturn]] void exit_current_process(int64_t exit_code) noexcept;
void attach_loaded_elf(Process& proc, Loader::LoadedElf&& loaded) noexcept;

} // namespace Kernel::Process
