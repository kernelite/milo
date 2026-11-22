#include "mocks/host_hal.hpp"

#include "hal/console.hpp"
#include "hal/cpu.hpp"
#include "hal/mmu.hpp"
#include "memory_config.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace {

constexpr size_t CONSOLE_BUFFER_CAP = 8192U;
constexpr size_t MAX_MOCK_ROOTS = 16U;
constexpr size_t MAX_MOCK_MAPPINGS = 512U;
constexpr uint16_t ELF_MACHINE_AARCH64 = 183U;
constexpr uintptr_t AARCH64_USER_VA_LIMIT = 0x0000000100000000ULL;
constexpr uintptr_t AARCH64_USER_STACK_TOP = 0x000000003FFF0000ULL;
constexpr uintptr_t AARCH64_USER_BRK_BASE = 0x0000000010000000ULL;
constexpr uintptr_t AARCH64_USER_MMAP_BASE = 0x000000003FFE0000ULL;

alignas(Arch::PAGE_SIZE) uint8_t g_sim_ram[HostTest::SIM_RAM_SIZE]{};

struct MockRootSlot {
    uintptr_t root_paddr{0U};
    bool in_use{false};
};

struct MockPageMapping {
    uintptr_t root_paddr{0U};
    uintptr_t vaddr{0U};
    uintptr_t paddr{0U};
    HAL::PageFlags flags{HAL::PageFlags::None};
    bool valid{false};
};

MockRootSlot g_roots[MAX_MOCK_ROOTS]{};
MockPageMapping g_mappings[MAX_MOCK_MAPPINGS]{};
uintptr_t g_current_root = 0U;
bool g_fail_create_root = false;
size_t g_map_calls_remaining = static_cast<size_t>(-1);
HostTest::MockStats g_stats{};
HAL::PlatformInfo g_platform_info{};

void init_default_platform_info() noexcept {
    const uintptr_t sim_host_base = reinterpret_cast<uintptr_t>(g_sim_ram);
    g_platform_info = HAL::PlatformInfo{
        .ram_base = HostTest::SIM_RAM_BASE,
        .ram_size = HostTest::SIM_RAM_SIZE,
        .max_ram_size = Arch::MAX_RAM_SIZE,
        .page_size = Arch::PAGE_SIZE,
        .higher_half_offset = sim_host_base - HostTest::SIM_RAM_BASE,
        .user_va_limit = AARCH64_USER_VA_LIMIT,
        .user_stack_top = AARCH64_USER_STACK_TOP,
        .user_brk_base = AARCH64_USER_BRK_BASE,
        .user_mmap_base = AARCH64_USER_MMAP_BASE,
        .elf_machine = ELF_MACHINE_AARCH64,
    };
}

class HostConsole final : public HAL::Console {
  public:
    void init() override { reset(); }

    void putc(char chr) override {
        ++g_stats.console_bytes_written;
        if (m_len + 1U < CONSOLE_BUFFER_CAP) {
            m_buf[m_len++] = chr;
            m_buf[m_len] = '\0';
        }
    }

    uint8_t getc() override { return 0U; }

    void reset() noexcept {
        m_len = 0U;
        m_buf[0] = '\0';
    }

    const char* data() const noexcept { return m_buf; }

  private:
    char m_buf[CONSOLE_BUFFER_CAP]{};
    size_t m_len{0U};
};

class HostCpuControl final : public HAL::CpuControl {
  public:
    void enable_interrupts() override { m_irq_enabled = true; }
    void disable_interrupts() override { m_irq_enabled = false; }
    bool interrupts_enabled() const override { return m_irq_enabled; }
    uint8_t current_el() const override { return 1U; }
    void halt() override {}

    void switch_context(HAL::CpuContext** old_ctx, HAL::CpuContext* new_ctx) override {
        (void)old_ctx;
        (void)new_ctx;
    }

    void switch_kernel_context(HAL::KernelContext* prev,
                               const HAL::KernelContext* next) noexcept override {
        (void)prev;
        (void)next;
    }

    void init_user_kstack(HAL::KernelContext& kctx, uintptr_t kstack_top,
                          const HAL::CpuContext& user_ctx) const noexcept override {
        (void)kctx;
        (void)kstack_top;
        (void)user_ctx;
    }

    void flush_dcache_range(uintptr_t virt_addr, size_t size) noexcept override {
        (void)virt_addr;
        (void)size;
        ++g_stats.dcache_flushes;
    }

    void invalidate_icache() noexcept override { ++g_stats.icache_invalidations; }

    void init_user_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t user_sp,
                           uint64_t arg0, uint64_t arg1) const noexcept override {
        (void)ctx;
        (void)entry_point;
        (void)user_sp;
        (void)arg0;
        (void)arg1;
    }

    int64_t enter_user_mode(uintptr_t entry_point, uintptr_t user_sp) noexcept override {
        (void)entry_point;
        (void)user_sp;
        return 0;
    }

    int64_t enter_user_context(const HAL::CpuContext& ctx) noexcept override {
        (void)ctx;
        return 0;
    }

    [[noreturn]] void return_to_kernel(int64_t exit_code) noexcept override {
        (void)exit_code;
        __builtin_trap();
    }

    uint64_t get_syscall_number(const HAL::CpuContext& ctx) const noexcept override {
        (void)ctx;
        return 0ULL;
    }

    uint64_t get_syscall_arg(const HAL::CpuContext& ctx, size_t index) const noexcept override {
        (void)ctx;
        (void)index;
        return 0ULL;
    }

    void set_syscall_return(HAL::CpuContext& ctx, int64_t value) const noexcept override {
        (void)ctx;
        (void)value;
    }

    void set_user_stack_pointer(HAL::CpuContext& ctx, uintptr_t user_sp) const noexcept override {
        (void)ctx;
        (void)user_sp;
    }

    void set_user_tls_pointer(HAL::CpuContext& ctx, uintptr_t tls) const noexcept override {
        (void)ctx;
        (void)tls;
    }

    uintptr_t get_user_tls_pointer(const HAL::CpuContext& ctx) const noexcept override {
        (void)ctx;
        return 0U;
    }

    uint64_t get_reg(const HAL::CpuContext& ctx, size_t index) const noexcept override {
        (void)ctx;
        (void)index;
        return 0ULL;
    }

    void set_reg(HAL::CpuContext& ctx, size_t index, uint64_t value) const noexcept override {
        (void)ctx;
        (void)index;
        (void)value;
    }

    uintptr_t get_user_stack_pointer(const HAL::CpuContext& ctx) const noexcept override {
        (void)ctx;
        return 0U;
    }

    uintptr_t get_user_pc(const HAL::CpuContext& ctx) const noexcept override {
        (void)ctx;
        return 0U;
    }

    void set_user_pc(HAL::CpuContext& ctx, uintptr_t program_counter) const noexcept override {
        (void)ctx;
        (void)program_counter;
    }

    uint64_t get_user_pstate(const HAL::CpuContext& ctx) const noexcept override {
        (void)ctx;
        return 0ULL;
    }

    void set_user_pstate(HAL::CpuContext& ctx, uint64_t pstate) const noexcept override {
        (void)ctx;
        (void)pstate;
    }

    void reset() noexcept { m_irq_enabled = false; }

  private:
    bool m_irq_enabled{false};
};

class HostMemoryControl final : public HAL::MemoryControl {
  public:
    bool map(uintptr_t virt_addr, uintptr_t phys_addr, HAL::PageFlags flags) override {
        (void)virt_addr;
        (void)phys_addr;
        (void)flags;
        return true;
    }

    bool unmap(uintptr_t virt_addr) override {
        (void)virt_addr;
        return true;
    }

    void activate() override {}

    HAL::MmuStatus status() const noexcept override {
        return HAL::MmuStatus{
            .mmu_enabled = true,
            .dcache_enabled = true,
            .icache_enabled = true,
            .raw_control_reg = 0ULL,
        };
    }
};

HostConsole g_console{};
HostCpuControl g_cpu{};
HostMemoryControl g_mmu{};

bool is_valid_root(uintptr_t root_paddr) noexcept {
    if (root_paddr == 0U) {
        return false;
    }
    return std::ranges::any_of(g_roots, [root_paddr](const MockRootSlot& slot) noexcept {
        return slot.in_use && slot.root_paddr == root_paddr;
    });
}

} // namespace

namespace HAL {

Console& get_console() noexcept {
    return g_console;
}

CpuControl& get_cpu() noexcept {
    return g_cpu;
}

MemoryControl& get_mmu() noexcept {
    return g_mmu;
}

PlatformInfo get_platform_info() noexcept {
    if (g_platform_info.page_size == 0U) {
        init_default_platform_info();
    }
    return g_platform_info;
}

void set_platform_memory(uintptr_t ram_base, size_t ram_size) noexcept {
    if (g_platform_info.page_size == 0U) {
        init_default_platform_info();
    }
    g_platform_info.ram_base = ram_base;
    g_platform_info.ram_size = ram_size;
}

uintptr_t AddressSpaceBackend::create_root() noexcept {
    if (g_fail_create_root) {
        return 0U;
    }
    for (size_t idx = 0U; idx < MAX_MOCK_ROOTS; ++idx) {
        if (!g_roots[idx].in_use) {
            g_roots[idx].in_use = true;
            g_roots[idx].root_paddr = HostTest::SIM_RAM_BASE + ((idx + 1U) * Arch::PAGE_SIZE);
            ++g_stats.active_address_spaces;
            return g_roots[idx].root_paddr;
        }
    }
    return 0U;
}

bool AddressSpaceBackend::clone_user_space(uintptr_t src_root, uintptr_t dst_root) noexcept {
    if (!is_valid_root(src_root) || !is_valid_root(dst_root)) {
        return false;
    }
    for (size_t idx = 0U; idx < MAX_MOCK_MAPPINGS; ++idx) {
        if (g_mappings[idx].valid && g_mappings[idx].root_paddr == src_root) {
            if (!map_user_page(dst_root, g_mappings[idx].vaddr, g_mappings[idx].paddr,
                               g_mappings[idx].flags)) {
                return false;
            }
        }
    }
    return true;
}

bool AddressSpaceBackend::map_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t paddr,
                                        PageFlags flags) noexcept {
    if (!is_valid_root(root_paddr)) {
        return false;
    }
    if (g_map_calls_remaining == 0U) {
        return false;
    }
    if (g_map_calls_remaining != static_cast<size_t>(-1)) {
        --g_map_calls_remaining;
    }

    const uintptr_t aligned_vaddr = vaddr & ~(Arch::PAGE_SIZE - 1U);
    for (const auto& entry : g_mappings) {
        if (entry.valid && entry.root_paddr == root_paddr && entry.vaddr == aligned_vaddr) {
            return false;
        }
    }

    for (auto& entry : g_mappings) {
        if (!entry.valid) {
            entry.valid = true;
            entry.root_paddr = root_paddr;
            entry.vaddr = aligned_vaddr;
            entry.paddr = paddr & ~(Arch::PAGE_SIZE - 1U);
            entry.flags = flags;
            ++g_stats.mapped_page_count;
            return true;
        }
    }
    return false;
}

bool AddressSpaceBackend::map_demand_zero(uintptr_t root_paddr, uintptr_t vaddr,
                                          PageFlags expected_flags) noexcept {
    return map_user_page(root_paddr, vaddr, 0U, expected_flags);
}

bool AddressSpaceBackend::resolve_cow_page(uintptr_t root_paddr, uintptr_t vaddr,
                                           uintptr_t& out_paddr) noexcept {
    PageFlags flags = PageFlags::None;
    return query_user_page(root_paddr, vaddr, out_paddr, flags);
}

bool AddressSpaceBackend::resolve_demand_zero_page(uintptr_t root_paddr, uintptr_t vaddr,
                                                   bool require_write,
                                                   uintptr_t& out_paddr) noexcept {
    (void)require_write;
    PageFlags flags = PageFlags::None;
    return query_user_page(root_paddr, vaddr, out_paddr, flags);
}

bool AddressSpaceBackend::unmap_user_page(uintptr_t root_paddr, uintptr_t vaddr) noexcept {
    if (!is_valid_root(root_paddr)) {
        return false;
    }
    const uintptr_t aligned_vaddr = vaddr & ~(Arch::PAGE_SIZE - 1U);
    for (auto& entry : g_mappings) {
        if (entry.valid && entry.root_paddr == root_paddr && entry.vaddr == aligned_vaddr) {
            entry.valid = false;
            if (g_stats.mapped_page_count > 0U) {
                --g_stats.mapped_page_count;
            }
            return true;
        }
    }
    return false;
}

bool AddressSpaceBackend::protect_user_page(uintptr_t root_paddr, uintptr_t vaddr,
                                            PageFlags flags) noexcept {
    if (!is_valid_root(root_paddr)) {
        return false;
    }
    const uintptr_t aligned_vaddr = vaddr & ~(Arch::PAGE_SIZE - 1U);
    for (auto& entry : g_mappings) {
        if (entry.valid && entry.root_paddr == root_paddr && entry.vaddr == aligned_vaddr) {
            entry.flags = flags;
            return true;
        }
    }
    return false;
}

bool AddressSpaceBackend::query_user_page(uintptr_t root_paddr, uintptr_t vaddr,
                                          uintptr_t& out_paddr, PageFlags& out_flags) noexcept {
    if (!is_valid_root(root_paddr)) {
        return false;
    }
    const uintptr_t aligned_vaddr = vaddr & ~(Arch::PAGE_SIZE - 1U);
    for (const auto& entry : g_mappings) {
        if (entry.valid && entry.root_paddr == root_paddr && entry.vaddr == aligned_vaddr) {
            out_paddr = entry.paddr;
            out_flags = entry.flags;
            return true;
        }
    }
    return false;
}

bool AddressSpaceBackend::query_pte_flags(uintptr_t root_paddr, uintptr_t vaddr,
                                          uint64_t& out_raw_pte) noexcept {
    uintptr_t paddr = 0U;
    PageFlags flags = PageFlags::None;
    if (!query_user_page(root_paddr, vaddr, paddr, flags)) {
        return false;
    }
    out_raw_pte = static_cast<uint64_t>(flags);
    return true;
}

bool AddressSpaceBackend::remap_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t paddr,
                                          PageFlags flags) noexcept {
    if (!is_valid_root(root_paddr)) {
        return false;
    }
    const uintptr_t aligned_vaddr = vaddr & ~(Arch::PAGE_SIZE - 1U);
    for (auto& entry : g_mappings) {
        if (entry.valid && entry.root_paddr == root_paddr && entry.vaddr == aligned_vaddr) {
            entry.paddr = paddr & ~(Arch::PAGE_SIZE - 1U);
            entry.flags = flags;
            return true;
        }
    }
    return false;
}

void AddressSpaceBackend::activate_root(uintptr_t root_paddr) noexcept {
    g_current_root = root_paddr;
}

uintptr_t AddressSpaceBackend::current_root_paddr() noexcept {
    return g_current_root;
}

void AddressSpaceBackend::destroy_root(uintptr_t root_paddr) noexcept {
    for (auto& entry : g_mappings) {
        if (entry.valid && entry.root_paddr == root_paddr) {
            entry.valid = false;
            if (g_stats.mapped_page_count > 0U) {
                --g_stats.mapped_page_count;
            }
        }
    }
    for (auto& slot : g_roots) {
        if (slot.in_use && slot.root_paddr == root_paddr) {
            slot.in_use = false;
            slot.root_paddr = 0U;
            if (g_stats.active_address_spaces > 0U) {
                --g_stats.active_address_spaces;
            }
        }
    }
    if (g_current_root == root_paddr) {
        g_current_root = 0U;
    }
}

} // namespace HAL

namespace HostTest {

void reset_host_hal() noexcept {
    init_default_platform_info();
    for (size_t idx = 0U; idx < SIM_RAM_SIZE; ++idx) {
        g_sim_ram[idx] = 0U;
    }
    for (auto& slot : g_roots) {
        slot = MockRootSlot{};
    }
    for (auto& entry : g_mappings) {
        entry = MockPageMapping{};
    }
    g_current_root = 0U;
    g_fail_create_root = false;
    g_map_calls_remaining = static_cast<size_t>(-1);
    g_stats = MockStats{};
    g_console.reset();
    g_cpu.reset();
}

void set_fail_create_root(bool should_fail) noexcept {
    g_fail_create_root = should_fail;
}

void set_fail_map_after(size_t successful_maps_before_failure) noexcept {
    g_map_calls_remaining = successful_maps_before_failure;
}

MockStats get_mock_stats() noexcept {
    return g_stats;
}

const char* get_captured_console_output() noexcept {
    return g_console.data();
}

bool lookup_mapped_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t& out_paddr,
                        HAL::PageFlags& out_flags) noexcept {
    return HAL::AddressSpaceBackend::query_user_page(root_paddr, vaddr, out_paddr, out_flags);
}

} // namespace HostTest