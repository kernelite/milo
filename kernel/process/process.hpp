#pragma once

#include "hal/cpu.hpp"
#include "ipc/pipe.hpp"
#include "loader/elf_loader.hpp"
#include "mm/address_space.hpp"
#include "process/resource.hpp"
#include "process/stack_builder.hpp"
#include "process/wait_queue.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Process {

constexpr size_t INITIAL_FD_CAPACITY = 16U;
constexpr size_t KERNEL_STACK_PAGES = 4U; // 16 KiB per-process kernel stack
constexpr size_t MAX_PATH_LEN = 256U;
constexpr size_t MAX_SIGNALS = 64U;

constexpr uint32_t FD_FLAG_READ = 1U << 0U;
constexpr uint32_t FD_FLAG_WRITE = 1U << 1U;
constexpr uint32_t FD_FLAG_NONBLOCK = 1U << 2U;
constexpr uint32_t FD_FLAG_CLOEXEC = 1U << 3U;

// Standard Linux Signal Numbers
constexpr uint32_t SIG_HUP = 1U;
constexpr uint32_t SIG_INT = 2U;
constexpr uint32_t SIG_QUIT = 3U;
constexpr uint32_t SIG_KILL = 9U;
constexpr uint32_t SIG_PIPE = 13U;
constexpr uint32_t SIG_ALRM = 14U;
constexpr uint32_t SIG_TERM = 15U;
constexpr uint32_t SIG_CHLD = 17U;
constexpr uint32_t SIG_CONT = 18U;
constexpr uint32_t SIG_STOP = 19U;
constexpr uint32_t SIG_TSTP = 20U;
constexpr uint32_t SIG_TTIN = 21U;
constexpr uint32_t SIG_TTOU = 22U;

constexpr uint64_t KERNEL_SIG_DFL = 0ULL;
constexpr uint64_t KERNEL_SIG_IGN = 1ULL;
constexpr uint64_t SA_FLAG_RESTORER = 0x04000000ULL;

struct SigAction {
    uint64_t handler{KERNEL_SIG_DFL};
    uint64_t flags{0ULL};
    uint64_t restorer{0ULL};
    uint64_t mask{0ULL};
};

enum class ProcessState : uint8_t { Unused = 0, Ready, Running, Blocked, Stopped, Zombie };

struct FileDescriptor {
    kernel::fs::File* file{nullptr};
    uint32_t fd_flags{0U};

    [[nodiscard]] bool is_open() const noexcept { return file != nullptr && file->active; }

    [[nodiscard]] bool is_readable() const noexcept {
        if (!is_open()) {
            return false;
        }
        const uint32_t acc = file->flags & kernel::fs::kOpenAccessMask;
        return acc == kernel::fs::kOpenReadOnly || acc == kernel::fs::kOpenReadWrite;
    }

    [[nodiscard]] bool is_writable() const noexcept {
        if (!is_open()) {
            return false;
        }
        const uint32_t acc = file->flags & kernel::fs::kOpenAccessMask;
        return acc == kernel::fs::kOpenWriteOnly || acc == kernel::fs::kOpenReadWrite;
    }

    [[nodiscard]] bool is_tty() const noexcept {
        return is_open() && file->inode != nullptr && file->inode->is_chr() &&
               file->f_ops != nullptr && file->f_ops->ioctl != nullptr;
    }
};

struct Process {
    ~Process() noexcept;

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

// Dynamic file descriptor table lifecycle & allocation
bool init_fd_table(Process& proc, size_t initial_capacity = INITIAL_FD_CAPACITY) noexcept;
void destroy_fd_table(Process& proc) noexcept;
bool ensure_fd_capacity(Process& proc, size_t required_slots) noexcept;
int allocate_fd(Process& proc, size_t min_fd = 0U) noexcept;
void close_fd(Process& proc, size_t fd_idx) noexcept;
void copy_fd_slot(FileDescriptor& dst, const FileDescriptor& src, bool clear_cloexec) noexcept;
void close_cloexec_fds(Process& proc) noexcept;

// Signal delivery & sigreturn unwinding
void send_signal(Process& target, uint32_t sig) noexcept;
void signal_process_group(uint32_t pgid, uint32_t sig) noexcept;
[[nodiscard]] bool has_unblocked_pending_signal(const Process& proc) noexcept;
void deliver_pending_signals(HAL::CpuContext& ctx) noexcept;
int64_t restore_signal_frame(HAL::CpuContext& ctx) noexcept;

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