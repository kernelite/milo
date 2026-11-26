#pragma once

#include "hal/cpu.hpp"
#include "loader/elf_loader.hpp"
#include "mm/AddressSpace.hpp"
#include "process/FdTable.hpp"
#include "process/Signal.hpp"
#include "process/resource.hpp"
#include "process/stack_builder.hpp"
#include "process/wait_queue.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Scheduler {
class Runqueue;
} // namespace Kernel::Scheduler

namespace Kernel::Process {

constexpr size_t KERNEL_STACK_PAGES = 4U; // 16 KiB per-process kernel stack
constexpr size_t MAX_PATH_LEN = 256U;

enum class ProcessState : uint8_t { Unused = 0, Ready, Running, Blocked, Stopped, Zombie };

class Process {
  public:
    Process() = default;
    ~Process() noexcept;

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) = delete;
    Process& operator=(Process&&) = delete;

    // --- Static Lifecycle & Global Process Table ---
    [[nodiscard]] static Process* create(uint32_t ppid = 0U) noexcept;
    static void destroy(Process& proc) noexcept;
    [[nodiscard]] static Process* find_by_pid(uint32_t pid) noexcept;
    [[nodiscard]] static Process* list_head() noexcept;
    [[nodiscard]] static size_t active_count() noexcept;
    [[nodiscard]] static bool has_runnable() noexcept;
    [[nodiscard]] static Process* current() noexcept;
    static void set_current(Process* proc) noexcept;

    // --- Identity & Job Control ---
    [[nodiscard]] constexpr uint32_t pid() const noexcept { return m_pid; }
    [[nodiscard]] constexpr uint32_t ppid() const noexcept { return m_ppid; }
    constexpr void set_ppid(uint32_t new_ppid) noexcept { m_ppid = new_ppid; }

    [[nodiscard]] constexpr uint32_t pgid() const noexcept { return m_pgid; }
    constexpr void set_pgid(uint32_t new_pgid) noexcept { m_pgid = new_pgid; }

    [[nodiscard]] constexpr uint32_t sid() const noexcept { return m_sid; }
    constexpr void set_sid(uint32_t new_sid) noexcept { m_sid = new_sid; }
    [[nodiscard]] bool setsid() noexcept;

    [[nodiscard]] constexpr uint32_t umask() const noexcept { return m_umask; }
    uint32_t set_umask(uint32_t new_mask) noexcept;

    // --- Lifecycle State & Scheduling ---
    [[nodiscard]] constexpr ProcessState state() const noexcept { return m_state; }
    constexpr void set_state(ProcessState new_state) noexcept { m_state = new_state; }

    [[nodiscard]] constexpr int64_t exit_status() const noexcept { return m_exit_status; }
    constexpr void set_exit_status(int64_t status) noexcept { m_exit_status = status; }

    void mark_stopped(uint32_t sig) noexcept;
    [[nodiscard]] uint32_t stop_signal() const noexcept { return m_stop_signal; }
    [[nodiscard]] bool is_stop_reported() const noexcept { return m_stop_reported; }
    [[nodiscard]] bool consume_stop_report(uint32_t& out_sig) noexcept;

    void mark_continued() noexcept;
    [[nodiscard]] bool is_continued() const noexcept { return m_continued; }
    [[nodiscard]] bool consume_continued() noexcept;

    [[nodiscard]] constexpr uint32_t timeslice() const noexcept { return m_timeslice; }
    constexpr void reset_timeslice(uint32_t ticks) noexcept { m_timeslice = ticks; }
    [[nodiscard]] bool tick_timeslice() noexcept;

    void record_cpu_tick(bool from_user) noexcept;
    void accumulate_child_ticks(const Process& child) noexcept;
    [[nodiscard]] constexpr size_t utime() const noexcept { return m_utime; }
    [[nodiscard]] constexpr size_t stime() const noexcept { return m_stime; }
    [[nodiscard]] constexpr size_t cutime() const noexcept { return m_cutime; }
    [[nodiscard]] constexpr size_t cstime() const noexcept { return m_cstime; }

    // --- Memory, Stack, Resource Limits & Paths ---
    [[nodiscard]] MM::AddressSpace& address_space() noexcept { return m_address_space; }
    [[nodiscard]] const MM::AddressSpace& address_space() const noexcept { return m_address_space; }

    [[nodiscard]] constexpr uintptr_t brk_start() const noexcept { return m_brk_start; }
    constexpr void set_brk_start(uintptr_t addr) noexcept { m_brk_start = addr; }
    [[nodiscard]] constexpr uintptr_t brk_current() const noexcept { return m_brk_current; }
    constexpr void set_brk_current(uintptr_t addr) noexcept { m_brk_current = addr; }

    [[nodiscard]] constexpr uintptr_t mmap_next_vaddr() const noexcept { return m_mmap_next_vaddr; }
    constexpr void set_mmap_next_vaddr(uintptr_t addr) noexcept { m_mmap_next_vaddr = addr; }

    [[nodiscard]] constexpr uintptr_t entry_point() const noexcept { return m_entry_point; }
    constexpr void set_entry_point(uintptr_t addr) noexcept { m_entry_point = addr; }

    [[nodiscard]] UserStack& user_stack() noexcept { return m_user_stack; }
    [[nodiscard]] const UserStack& user_stack() const noexcept { return m_user_stack; }
    void set_user_stack(const UserStack& stack) noexcept { m_user_stack = stack; }

    void attach_elf(Loader::LoadedElf&& loaded) noexcept;

    [[nodiscard]] const char* cwd() const noexcept { return m_cwd; }
    [[nodiscard]] char* mutable_cwd() noexcept { return m_cwd; }
    void set_cwd(const char* path) noexcept;

    [[nodiscard]] const char* exe_path() const noexcept { return m_exe_path; }
    void set_exe_path(const char* path) noexcept;

    [[nodiscard]] constexpr RLimit64 rlim_nofile() const noexcept {
        return RLimit64{.rlim_cur = m_rlim_nofile_cur, .rlim_max = m_rlim_nofile_max};
    }
    constexpr void set_rlim_nofile(const RLimit64& lim) noexcept {
        m_rlim_nofile_cur = lim.rlim_cur;
        m_rlim_nofile_max = lim.rlim_max;
    }

    [[nodiscard]] constexpr RLimit64 rlim_nproc() const noexcept {
        return RLimit64{.rlim_cur = m_rlim_nproc_cur, .rlim_max = m_rlim_nproc_max};
    }
    constexpr void set_rlim_nproc(const RLimit64& lim) noexcept {
        m_rlim_nproc_cur = lim.rlim_cur;
        m_rlim_nproc_max = lim.rlim_max;
    }

    [[nodiscard]] constexpr uintptr_t clear_child_tid() const noexcept { return m_clear_child_tid; }
    constexpr void set_clear_child_tid(uintptr_t tidptr) noexcept { m_clear_child_tid = tidptr; }

    // --- Subsystem Accessors & Execution ---
    [[nodiscard]] FdTable& fds() noexcept { return m_fds; }
    [[nodiscard]] const FdTable& fds() const noexcept { return m_fds; }

    [[nodiscard]] SignalTable& signals() noexcept { return m_signals; }
    [[nodiscard]] const SignalTable& signals() const noexcept { return m_signals; }

    [[nodiscard]] WaitQueue& child_wait_queue() noexcept { return m_child_wait_queue; }
    [[nodiscard]] const WaitQueue& child_wait_queue() const noexcept { return m_child_wait_queue; }

    [[nodiscard]] HAL::CpuContext& context() noexcept { return m_context; }
    [[nodiscard]] const HAL::CpuContext& context() const noexcept { return m_context; }

    [[nodiscard]] HAL::KernelContext& kctx() noexcept { return m_kctx; }
    [[nodiscard]] const HAL::KernelContext& kctx() const noexcept { return m_kctx; }

    [[nodiscard]] constexpr uintptr_t kstack_top() const noexcept { return m_kstack_top; }
    [[nodiscard]] constexpr bool is_kctx_initialized() const noexcept { return m_kctx_initialized; }
    constexpr void set_kctx_initialized(bool initialized) noexcept {
        m_kctx_initialized = initialized;
    }

    void prepare_kstack() noexcept;
    int64_t execute() noexcept;

    [[nodiscard]] Process* next_global() const noexcept { return m_global_next; }
    [[nodiscard]] WaitQueue* active_wait_queue() const noexcept { return m_active_wq; }

  private:
    friend class Kernel::Scheduler::Runqueue;
    friend class WaitQueue;

    HAL::KernelContext m_kctx{};
    HAL::CpuContext m_context{};
    int64_t m_exit_status{0};

    // Integrated CPU accounting counters (in timer ticks)
    size_t m_utime{0U};
    size_t m_stime{0U};
    size_t m_cutime{0U};
    size_t m_cstime{0U};

    uint64_t m_rlim_nofile_cur{DEFAULT_RLIMIT_NOFILE_CUR};
    uint64_t m_rlim_nofile_max{DEFAULT_RLIMIT_NOFILE_MAX};
    uint64_t m_rlim_nproc_cur{0ULL};
    uint64_t m_rlim_nproc_max{0ULL};
    MM::AddressSpace m_address_space;
    uintptr_t m_kstack_paddr{0U};
    uintptr_t m_kstack_top{0U};
    uintptr_t m_entry_point{0U};
    uintptr_t m_brk_start{0U};
    uintptr_t m_brk_current{0U};
    uintptr_t m_mmap_next_vaddr{0U};
    uintptr_t m_clear_child_tid{0U};
    UserStack m_user_stack{};
    WaitQueue m_child_wait_queue;
    SignalTable m_signals;
    FdTable m_fds;
    uint32_t m_pid{0U};
    uint32_t m_ppid{0U};
    uint32_t m_pgid{0U};
    uint32_t m_sid{0U};
    uint32_t m_umask{0022U};
    uint32_t m_timeslice{0U};
    uint32_t m_stop_signal{0U};
    ProcessState m_state{ProcessState::Unused};
    bool m_stop_reported{false};
    bool m_continued{false};
    bool m_kctx_initialized{false};
    char m_cwd[MAX_PATH_LEN]{};
    char m_exe_path[MAX_PATH_LEN]{};

    // Intrusive global process list links
    Process* m_global_next{nullptr};
    Process* m_global_prev{nullptr};

    // Intrusive CPU runqueue links
    Process* m_rq_next{nullptr};
    Process* m_rq_prev{nullptr};
    bool m_on_runqueue{false};

    // Intrusive waitqueue links
    Process* m_wq_next{nullptr};
    Process* m_wq_prev{nullptr};
    WaitQueue* m_active_wq{nullptr};
};

uint64_t last_cpu_ticks_for_pid(uint32_t pid) noexcept;

// Console TTY polling & foreground job-control group helpers
void poll_console_tty() noexcept;
void set_foreground_pgid(uint32_t pgid) noexcept;
[[nodiscard]] uint32_t get_foreground_pgid() noexcept;

void init_user_cpu_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t sp_el0,
                           uint64_t argc, uintptr_t argv_vaddr) noexcept;

inline int64_t enter_user_context(const HAL::CpuContext* ctx) noexcept {
    if (ctx == nullptr) {
        return -1;
    }
    return HAL::get_cpu().enter_user_context(*ctx);
}

[[noreturn]] void exit_current_process(int64_t exit_code) noexcept;

} // namespace Kernel::Process
