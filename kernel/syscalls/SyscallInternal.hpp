#pragma once

#include "fs/Vfs.hpp"
#include "hal/mmu.hpp"
#include "mm/AddressSpace.hpp"
#include "process/Process.hpp"
#include "uapi/errno.hpp"
#include "uapi/fs.hpp"
#include "uapi/signal.hpp"
#include "uapi/sys.hpp"
#include "uapi/time.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Syscalls {

using namespace Kernel::UAPI;

// Internal syscall buffer & arena limits
constexpr size_t EXEC_MAX_PATH_LEN = 256U;
constexpr size_t EXEC_MAX_ARGS = 64U;
constexpr size_t EXEC_MAX_ENVS = 64U;

constexpr uintptr_t MMAP_ARENA_FLOOR = 0x0000000020000000ULL;

constexpr size_t READ_CHUNK_SIZE = 256U;
constexpr size_t WRITE_CHUNK_SIZE = 256U;

constexpr size_t MAX_FDSET_FDS = FD_SETSIZE;
constexpr size_t MAX_FDSET_WORDS = MAX_FDSET_FDS / 64U;

constexpr size_t POLL_STACK_THRESHOLD = 32U;

// Transitional ABI type aliases (removed in Sub-Issue 1B)
using UserIovec = ::linux_iovec;
using LinuxStat = ::linux_stat;
using LinuxDirent64Header = ::linux_dirent64_hdr;
using LinuxSigAction = ::linux_sigaction;
using LinuxTms = ::linux_tms;
using LinuxUtsname = ::linux_utsname;
using LinuxTimespec = ::linux_timespec;
using LinuxTimeval = ::linux_timeval;
using LinuxRusage = ::linux_rusage;
using LinuxRlimit64 = ::linux_rlimit64;
using LinuxPollFd = ::linux_pollfd;
using LinuxSigsetArgpack = ::linux_sigset_argpack;
using LinuxSysinfo = ::linux_sysinfo;

// Shared syscall helper functions
void wait_for_event_or_tick() noexcept;
[[nodiscard]] uint64_t timespec_to_timer_ticks(const LinuxTimespec& time_spec,
                                               uint64_t freq) noexcept;
[[nodiscard]] uint64_t monotonic_now_ns() noexcept;

[[nodiscard]] size_t cstr_len(const char* str) noexcept;
void cstr_copy(char* dst, const char* src, size_t cap) noexcept;
[[nodiscard]] bool cstr_eq(const char* lhs, const char* rhs) noexcept;

[[nodiscard]] bool normalize_vfs_path(const char* base_dir, const char* input, char* out,
                                      size_t out_cap) noexcept;
[[nodiscard]] Kernel::FS::File* get_proc_file(Kernel::Process::Process* proc,
                                              uint64_t raw_fd) noexcept;
void fill_stat_from_inode(const Kernel::FS::Inode& inode, LinuxStat& stat_out) noexcept;

void unmap_and_free_range(Kernel::MM::AddressSpace& adrsp, uintptr_t start_vaddr,
                          uintptr_t end_vaddr, size_t page_size) noexcept;
[[nodiscard]] bool validate_user_range(uintptr_t root_paddr, uintptr_t user_buf, size_t count,
                                       HAL::PageFlags required_perm) noexcept;
[[nodiscard]] bool validate_user_read_range(uintptr_t root_paddr, uintptr_t user_buf,
                                            size_t count) noexcept;
[[nodiscard]] bool validate_user_write_range(uintptr_t root_paddr, uintptr_t user_buf,
                                             size_t count) noexcept;

[[nodiscard]] int64_t copy_user_cstring(uintptr_t root_paddr, uintptr_t user_str, char* dst,
                                        size_t max_len, size_t& out_len) noexcept;
[[nodiscard]] int64_t stage_user_string_array(uintptr_t root_paddr, uintptr_t user_array,
                                              const char** out_ptrs, size_t max_entries, char* pool,
                                              size_t pool_cap, size_t& pool_offset,
                                              int& out_count) noexcept;

[[nodiscard]] int64_t resolve_dirfd_dentry(Kernel::Process::Process& proc, int32_t dirfd,
                                           Kernel::FS::Dentry*& out_base) noexcept;
void build_dentry_path(const Kernel::FS::Dentry* target, char* out, size_t cap) noexcept;

// Memory management syscalls (MemorySyscalls.cpp)
[[nodiscard]] int64_t sys_brk(uintptr_t addr) noexcept;
[[nodiscard]] int64_t sys_mmap(uintptr_t addr, uint64_t length, uint64_t prot, uint64_t flags,
                               uint64_t raw_fd, uint64_t offset) noexcept;
[[nodiscard]] int64_t sys_munmap(uintptr_t addr, uint64_t length) noexcept;
[[nodiscard]] int64_t sys_mprotect(uintptr_t addr, uint64_t length, uint64_t prot) noexcept;

// Time & system information syscalls (TimeSyscalls.cpp)
[[nodiscard]] int64_t sys_clock_gettime(uint64_t clk_id, uintptr_t user_tp) noexcept;
[[nodiscard]] int64_t sys_nanosleep(uintptr_t user_req, uintptr_t user_rem) noexcept;
[[nodiscard]] int64_t sys_times(uintptr_t user_buf) noexcept;
[[nodiscard]] int64_t sys_getrusage(int32_t who, uintptr_t user_usage) noexcept;
[[nodiscard]] int64_t sys_sysinfo(uintptr_t user_info) noexcept;
[[nodiscard]] int64_t sys_getrandom(uintptr_t user_buf, uint64_t raw_buflen,
                                    uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_uname(uintptr_t user_buf) noexcept;

// Signal syscalls (SignalSyscalls.cpp)
[[nodiscard]] int64_t sys_rt_sigaction(uint64_t sig, uintptr_t user_act, uintptr_t user_oldact,
                                       uint64_t sigsetsize) noexcept;
[[nodiscard]] int64_t sys_rt_sigprocmask(uint64_t how, uintptr_t user_set, uintptr_t user_oldset,
                                         uint64_t sigsetsize) noexcept;
[[nodiscard]] int64_t sys_rt_sigreturn(HAL::CpuContext& ctx) noexcept;
[[nodiscard]] int64_t sys_kill(int32_t pid, uint64_t sig) noexcept;

// Process lifecycle & job-control syscalls (ProcessSyscalls.cpp)
[[noreturn]] void sys_exit(uint64_t raw_status) noexcept;
[[nodiscard]] int64_t sys_clone(HAL::CpuContext& parent_ctx, uint64_t flags, uintptr_t child_stack,
                                uintptr_t parent_tid, uint64_t tls, uintptr_t child_tid) noexcept;
[[nodiscard]] int64_t sys_execve(HAL::CpuContext& ctx, uintptr_t user_pathname, uintptr_t user_argv,
                                 uintptr_t user_envp) noexcept;
[[nodiscard]] int64_t sys_wait4(int32_t target_pid, uintptr_t user_wstatus, uint64_t options,
                                uintptr_t user_rusage) noexcept;
[[nodiscard]] int64_t sys_getpid() noexcept;
[[nodiscard]] int64_t sys_getppid() noexcept;
[[nodiscard]] int64_t sys_set_tid_address(uintptr_t tidptr) noexcept;
[[nodiscard]] int64_t sys_setpgid(int32_t raw_pid, int32_t raw_pgid) noexcept;
[[nodiscard]] int64_t sys_getpgid(int32_t raw_pid) noexcept;
[[nodiscard]] int64_t sys_getsid(int32_t raw_pid) noexcept;
[[nodiscard]] int64_t sys_setsid() noexcept;
[[nodiscard]] int64_t sys_umask(uint64_t mask) noexcept;
[[nodiscard]] int64_t sys_prlimit64(int32_t pid, uint32_t resource, uintptr_t user_new_rlim,
                                    uintptr_t user_old_rlim) noexcept;

// VFS, directory, pipe, FD, TTY & I/O syscalls (FileSyscalls.cpp)
[[nodiscard]] int64_t sys_getcwd(uintptr_t user_buf, uint64_t size) noexcept;
[[nodiscard]] int64_t sys_chdir(uintptr_t user_path) noexcept;
[[nodiscard]] int64_t sys_openat(int32_t dirfd, uintptr_t user_pathname, uint64_t flags,
                                 uint64_t mode) noexcept;
[[nodiscard]] int64_t sys_mkdirat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode) noexcept;
[[nodiscard]] int64_t sys_unlinkat(int32_t dirfd, uintptr_t user_pathname, uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_renameat2(int32_t olddirfd, uintptr_t user_oldpath, int32_t newdirfd,
                                    uintptr_t user_newpath, uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_renameat(int32_t olddirfd, uintptr_t user_oldpath, int32_t newdirfd,
                                   uintptr_t user_newpath) noexcept;
[[nodiscard]] int64_t sys_ftruncate(uint64_t raw_fd, int64_t length) noexcept;
[[nodiscard]] int64_t sys_truncate(uintptr_t user_pathname, int64_t length) noexcept;
[[nodiscard]] int64_t sys_utimensat(int32_t dirfd, uintptr_t user_pathname, uintptr_t user_times,
                                    uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_fsync(uint64_t raw_fd, int32_t datasync) noexcept;
[[nodiscard]] int64_t sys_fchmod(uint64_t raw_fd, uint64_t mode) noexcept;
[[nodiscard]] int64_t sys_fchmodat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                                   uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_fchown(uint64_t raw_fd, uint32_t owner, uint32_t group) noexcept;
[[nodiscard]] int64_t sys_fchownat(int32_t dirfd, uintptr_t user_pathname, uint32_t owner,
                                   uint32_t group, uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_close(uint64_t raw_fd) noexcept;
[[nodiscard]] int64_t sys_lseek(uint64_t raw_fd, int64_t offset, uint64_t whence) noexcept;
[[nodiscard]] int64_t sys_fstat(uint64_t raw_fd, uintptr_t user_statbuf) noexcept;
[[nodiscard]] int64_t sys_newfstatat(int32_t dirfd, uintptr_t user_pathname, uintptr_t user_statbuf,
                                     uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_faccessat2(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                                     uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_faccessat(int32_t dirfd, uintptr_t user_pathname, uint64_t mode,
                                    uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_readlinkat(int32_t dirfd, uintptr_t user_pathname, uintptr_t user_buf,
                                     uint64_t bufsiz) noexcept;
[[nodiscard]] int64_t sys_getdents64(uint64_t raw_fd, uintptr_t user_dirp, uint64_t count) noexcept;
[[nodiscard]] int64_t sys_dup(uint64_t raw_oldfd) noexcept;
[[nodiscard]] int64_t sys_dup3(uint64_t raw_oldfd, uint64_t raw_newfd, uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_fcntl(uint64_t raw_fd, uint64_t cmd, uint64_t arg) noexcept;
[[nodiscard]] int64_t sys_pipe2(uintptr_t user_pipefd, uint64_t flags) noexcept;
[[nodiscard]] int64_t sys_ioctl(uint64_t raw_fd, uint64_t request, uintptr_t arg) noexcept;
[[nodiscard]] int64_t sys_read(uint64_t raw_fd, uintptr_t user_buf, uint64_t raw_count) noexcept;
[[nodiscard]] int64_t sys_write(uint64_t raw_fd, uintptr_t user_buf, uint64_t raw_count) noexcept;
[[nodiscard]] int64_t sys_writev(uint64_t raw_fd, uintptr_t user_iov, uint64_t raw_iovcnt) noexcept;
[[nodiscard]] int64_t sys_ppoll(uintptr_t user_fds, uint64_t nfds, uintptr_t user_tmo,
                                uintptr_t user_sigmask, uint64_t sigsetsize) noexcept;
[[nodiscard]] int64_t sys_pselect6(int32_t nfds, uintptr_t user_rfds, uintptr_t user_wfds,
                                   uintptr_t user_efds, uintptr_t user_tmo,
                                   uintptr_t user_sigpack) noexcept;

} // namespace Kernel::Syscalls
