#pragma once
#include <cstddef>
#include <cstdint>

namespace HAL {

// Opaque 288-byte architecture trap/execution register frame.
struct alignas(16) CpuContext {
    uint64_t opaque[36];
};
static_assert(sizeof(CpuContext) == 288U, "CpuContext must be 288 bytes");

// Opaque 112-byte EL1 callee-saved kernel thread context (x19..x30 + sp).
struct alignas(16) KernelContext {
    uint64_t opaque[14];
};
static_assert(sizeof(KernelContext) == 112U, "KernelContext must be 112 bytes");

struct PlatformInfo {
    uintptr_t ram_base;
    size_t ram_size;
    size_t max_ram_size;
    size_t page_size;
    uintptr_t higher_half_offset;
    uintptr_t user_va_limit;
    uintptr_t user_stack_top;
    uintptr_t user_brk_base;
    uintptr_t user_mmap_base;
    uint16_t elf_machine;
};

class CpuControl {
  public:
    constexpr CpuControl() = default;
    virtual ~CpuControl() = default;

    virtual void enable_interrupts() = 0;
    virtual void disable_interrupts() = 0;
    virtual bool interrupts_enabled() const = 0;

    virtual uint8_t current_el() const = 0;
    virtual void halt() = 0;

    virtual void switch_context(CpuContext** old_ctx, CpuContext* new_ctx) = 0;
    virtual void switch_kernel_context(KernelContext* prev, const KernelContext* next) noexcept = 0;
    virtual void init_user_kstack(KernelContext& kctx, uintptr_t kstack_top,
                                  const CpuContext& user_ctx) const noexcept = 0;

    // --- Cache Operations ---
    virtual void flush_dcache_range(uintptr_t virt_addr, size_t size) noexcept = 0;
    virtual void invalidate_icache() noexcept = 0;

    // --- Execution Context & ABI Register Accessors ---
    virtual void init_user_context(CpuContext& ctx, uintptr_t entry_point, uintptr_t user_sp,
                                   uint64_t arg0, uint64_t arg1) const noexcept = 0;
    virtual int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp) noexcept = 0;
    virtual int64_t enter_user_context(const CpuContext& ctx) noexcept = 0;
    [[noreturn]] virtual void return_to_kernel(int64_t exit_code) noexcept = 0;

    virtual uint64_t get_syscall_number(const CpuContext& ctx) const noexcept = 0;
    virtual uint64_t get_syscall_arg(const CpuContext& ctx, size_t index) const noexcept = 0;
    virtual void set_syscall_return(CpuContext& ctx, int64_t value) const noexcept = 0;
    virtual void set_user_stack_pointer(CpuContext& ctx, uintptr_t user_sp) const noexcept = 0;
    virtual void set_user_tls_pointer(CpuContext& ctx, uintptr_t tls) const noexcept = 0;
    virtual uintptr_t get_user_tls_pointer(const CpuContext& ctx) const noexcept = 0;
};

CpuControl& get_cpu() noexcept;
PlatformInfo get_platform_info() noexcept;

} // namespace HAL