#include "mocks/host_hal.hpp"

#include "mm/pfa.hpp"

#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unordered_map>

extern "C" {
char _text_start[1] = {0};
// NOLINTNEXTLINE(bugprone-reserved-identifier)
char __bss_end[1] = {0};
}

namespace {

struct PageMapping {
    uintptr_t paddr{0U};
    HAL::PageFlags flags{HAL::PageFlags::None};
};

struct MockAddressSpaceTable {
    std::unordered_map<uintptr_t, PageMapping> pages;
};

class SimulatedRamBacking {
  public:
    SimulatedRamBacking() noexcept {
        void* ptr = mmap(nullptr, HostHAL::SIMULATED_RAM_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr != MAP_FAILED) {
            m_base = static_cast<uint8_t*>(ptr);
        }
    }

    ~SimulatedRamBacking() noexcept {
        if (m_base != nullptr) {
            munmap(m_base, HostHAL::SIMULATED_RAM_SIZE);
            m_base = nullptr;
        }
    }

    SimulatedRamBacking(const SimulatedRamBacking&) = delete;
    SimulatedRamBacking& operator=(const SimulatedRamBacking&) = delete;

    [[nodiscard]] uint8_t* base() const noexcept { return m_base; }

    [[nodiscard]] uintptr_t higher_half_offset() const noexcept {
        return reinterpret_cast<uintptr_t>(m_base) - HostHAL::SIMULATED_RAM_BASE;
    }

  private:
    uint8_t* m_base{nullptr};
};

SimulatedRamBacking& get_ram_backing() noexcept {
    static SimulatedRamBacking backing{};
    return backing;
}

class HostConsole final : public HAL::Console {
  public:
    void init() override { m_initialized = true; }

    void putc(char chr) override { m_buffer.push_back(chr); }

    uint8_t getc() override {
        ++m_getc_calls;
        return 0U;
    }

    bool has_input() const noexcept override { return m_has_input; }

    void clear() noexcept { m_buffer.clear(); }

    [[nodiscard]] const std::string& output() const noexcept { return m_buffer; }

  private:
    std::string m_buffer{};
    bool m_initialized{false};
    bool m_has_input{false};
    size_t m_getc_calls{0U};
};

class HostCpuControl final : public HAL::CpuControl {
  public:
    void enable_interrupts() override { m_irqs_enabled = true; }
    void disable_interrupts() override { m_irqs_enabled = false; }
    bool interrupts_enabled() const override { return m_irqs_enabled; }

    uint8_t current_el() const override { return 1U; }
    void halt() override { m_halt_count++; }

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
        m_dcache_flushes++;
    }

    void invalidate_icache() noexcept override { m_icache_invalidations++; }

    void init_user_context(HAL::CpuContext& ctx, uintptr_t entry_point, uintptr_t user_sp,
                           uint64_t arg0, uint64_t arg1) const noexcept override {
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.opaque[0] = arg0;
        ctx.opaque[1] = arg1;
        ctx.opaque[31] = user_sp;
        ctx.opaque[32] = entry_point;
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
        std::exit(static_cast<int>(exit_code));
    }

    uint64_t get_syscall_number(const HAL::CpuContext& ctx) const noexcept override {
        return ctx.opaque[8];
    }

    uint64_t get_syscall_arg(const HAL::CpuContext& ctx, size_t index) const noexcept override {
        return (index < 6U) ? ctx.opaque[index] : 0ULL;
    }

    void set_syscall_return(HAL::CpuContext& ctx, int64_t value) const noexcept override {
        ctx.opaque[0] = static_cast<uint64_t>(value);
    }

    void set_user_stack_pointer(HAL::CpuContext& ctx, uintptr_t user_sp) const noexcept override {
        ctx.opaque[31] = user_sp;
    }

    void set_user_tls_pointer(HAL::CpuContext& ctx, uintptr_t tls) const noexcept override {
        ctx.opaque[34] = tls;
    }

    uintptr_t get_user_tls_pointer(const HAL::CpuContext& ctx) const noexcept override {
        return static_cast<uintptr_t>(ctx.opaque[34]);
    }

    uint64_t get_reg(const HAL::CpuContext& ctx, size_t index) const noexcept override {
        return (index < 31U) ? ctx.opaque[index] : 0ULL;
    }

    void set_reg(HAL::CpuContext& ctx, size_t index, uint64_t value) const noexcept override {
        if (index < 31U) {
            ctx.opaque[index] = value;
        }
    }

    uintptr_t get_user_stack_pointer(const HAL::CpuContext& ctx) const noexcept override {
        return static_cast<uintptr_t>(ctx.opaque[31]);
    }

    uintptr_t get_user_pc(const HAL::CpuContext& ctx) const noexcept override {
        return static_cast<uintptr_t>(ctx.opaque[32]);
    }

    void set_user_pc(HAL::CpuContext& ctx, uintptr_t program_counter) const noexcept override {
        ctx.opaque[32] = program_counter;
    }

    uint64_t get_user_pstate(const HAL::CpuContext& ctx) const noexcept override {
        return ctx.opaque[33];
    }

    void set_user_pstate(HAL::CpuContext& ctx, uint64_t pstate) const noexcept override {
        ctx.opaque[33] = pstate;
    }

    size_t dcache_flushes() const noexcept { return m_dcache_flushes; }
    size_t icache_invalidations() const noexcept { return m_icache_invalidations; }

    void reset_counters() noexcept {
        m_irqs_enabled = false;
        m_halt_count = 0U;
        m_dcache_flushes = 0U;
        m_icache_invalidations = 0U;
    }

  private:
    bool m_irqs_enabled{false};
    size_t m_halt_count{0U};
    size_t m_dcache_flushes{0U};
    size_t m_icache_invalidations{0U};
};

class HostMemoryControl final : public HAL::MemoryControl {
  public:
    bool map(uintptr_t virt_addr, uintptr_t phys_addr, HAL::PageFlags flags) override {
        (void)virt_addr;
        (void)phys_addr;
        (void)flags;
        ++m_map_count;
        return true;
    }

    bool unmap(uintptr_t virt_addr) override { return m_kernel_mappings.erase(virt_addr) > 0U; }
    void activate() override { m_activated = true; }

    HAL::MmuStatus status() const noexcept override {
        return HAL::MmuStatus{
            .mmu_enabled = m_activated,
            .dcache_enabled = true,
            .icache_enabled = true,
            .raw_control_reg = m_activated ? 0x1005ULL : 0ULL,
        };
    }

    void reset() noexcept {
        m_kernel_mappings.clear();
        m_activated = false;
    }

  private:
    std::unordered_map<uintptr_t, PageMapping> m_kernel_mappings;
    bool m_activated{false};
    size_t m_map_count{0U};
};

HostConsole& host_console() noexcept {
    static HostConsole console{};
    return console;
}

HostCpuControl& host_cpu() noexcept {
    static HostCpuControl cpu{};
    return cpu;
}

HostMemoryControl& host_mmu() noexcept {
    static HostMemoryControl mmu{};
    return mmu;
}

std::unordered_map<uintptr_t, MockAddressSpaceTable>& address_space_tables() noexcept {
    static std::unordered_map<uintptr_t, MockAddressSpaceTable> tables{};
    return tables;
}

uintptr_t g_current_root_paddr = 0U;
uintptr_t g_synthetic_root_counter = 1U;

} // namespace

namespace HAL {

Console& get_console() noexcept {
    return host_console();
}

CpuControl& get_cpu() noexcept {
    return host_cpu();
}

MemoryControl& get_mmu() noexcept {
    return host_mmu();
}

PlatformInfo get_platform_info() noexcept {
    const auto& ram = get_ram_backing();
    return PlatformInfo{
        .ram_base = HostHAL::SIMULATED_RAM_BASE,
        .ram_size = HostHAL::SIMULATED_RAM_SIZE,
        .max_ram_size = HostHAL::SIMULATED_MAX_RAM_SIZE,
        .page_size = HostHAL::SIMULATED_PAGE_SIZE,
        .higher_half_offset = ram.higher_half_offset(),
        .user_va_limit = 0x0000000040000000ULL,
        .user_stack_top = 0x000000003FFF0000ULL,
        .user_brk_base = 0x0000000010000000ULL,
        .user_mmap_base = 0x000000003FFE0000ULL,
        .elf_machine = 183U,
    };
}

uintptr_t AddressSpaceBackend::create_root() noexcept {
    uintptr_t root_phys = 0U;
    if (pfa_free_pages_count() > 0U || pfa_allocated_pages_count() > 0U) {
        root_phys = alloc_frame();
        if (root_phys == 0U) {
            return 0U;
        }
    } else {
        root_phys = 0x10000000ULL + (g_synthetic_root_counter++ * HostHAL::SIMULATED_PAGE_SIZE);
    }

    address_space_tables()[root_phys] = MockAddressSpaceTable{};
    return root_phys;
}

bool AddressSpaceBackend::map_user_page(uintptr_t root_paddr, uintptr_t vaddr, uintptr_t paddr,
                                        PageFlags flags) noexcept {
    const auto info = get_platform_info();
    const uintptr_t page_mask = info.page_size - 1U;

    if (root_paddr == 0U || vaddr >= info.user_va_limit) {
        return false;
    }
    if ((vaddr & page_mask) != 0U || (paddr & page_mask) != 0U) {
        return false;
    }

    auto& tables = address_space_tables();
    auto iter = tables.find(root_paddr);
    if (iter == tables.end()) {
        return false;
    }

    iter->second.pages[vaddr] = PageMapping{.paddr = paddr, .flags = flags};
    return true;
}

bool AddressSpaceBackend::unmap_user_page(uintptr_t root_paddr, uintptr_t vaddr) noexcept {
    const auto info = get_platform_info();
    const uintptr_t page_mask = info.page_size - 1U;

    if (root_paddr == 0U || vaddr >= info.user_va_limit) {
        return false;
    }

    auto& tables = address_space_tables();
    auto iter = tables.find(root_paddr);
    if (iter == tables.end()) {
        return false;
    }

    return iter->second.pages.erase(vaddr & ~page_mask) > 0U;
}

bool AddressSpaceBackend::protect_user_page(uintptr_t root_paddr, uintptr_t vaddr,
                                            PageFlags flags) noexcept {
    const auto info = get_platform_info();
    const uintptr_t page_mask = info.page_size - 1U;

    if (root_paddr == 0U || vaddr >= info.user_va_limit) {
        return false;
    }

    auto& tables = address_space_tables();
    auto iter = tables.find(root_paddr);
    if (iter == tables.end()) {
        return false;
    }

    auto page_iter = iter->second.pages.find(vaddr & ~page_mask);
    if (page_iter == iter->second.pages.end()) {
        return false;
    }

    page_iter->second.flags = flags;
    return true;
}

bool AddressSpaceBackend::query_user_page(uintptr_t root_paddr, uintptr_t vaddr,
                                          uintptr_t& out_paddr, PageFlags& out_flags) noexcept {
    const auto info = get_platform_info();
    const uintptr_t page_mask = info.page_size - 1U;

    if (root_paddr == 0U || vaddr >= info.user_va_limit) {
        return false;
    }

    const auto& tables = address_space_tables();
    const auto iter = tables.find(root_paddr);
    if (iter == tables.end()) {
        return false;
    }

    const auto page_iter = iter->second.pages.find(vaddr & ~page_mask);
    if (page_iter == iter->second.pages.end()) {
        return false;
    }

    if (!has_flag(page_iter->second.flags, PageFlags::User)) {
        return false;
    }

    out_paddr = page_iter->second.paddr | (vaddr & page_mask);
    out_flags = page_iter->second.flags;
    return true;
}

void AddressSpaceBackend::activate_root(uintptr_t root_paddr) noexcept {
    g_current_root_paddr = root_paddr;
}

uintptr_t AddressSpaceBackend::current_root_paddr() noexcept {
    return g_current_root_paddr;
}

void AddressSpaceBackend::destroy_root(uintptr_t root_paddr) noexcept {
    if (root_paddr == 0U) {
        return;
    }

    auto& tables = address_space_tables();
    auto iter = tables.find(root_paddr);
    if (iter != tables.end()) {
        for (const auto& kv : iter->second.pages) {
            const uintptr_t page_paddr = kv.second.paddr;
            if (pfa_is_page_allocated(page_paddr)) {
                free_frame(page_paddr);
            }
        }
        tables.erase(iter);
    }

    if (pfa_is_page_allocated(root_paddr)) {
        free_frame(root_paddr);
    }

    if (g_current_root_paddr == root_paddr) {
        g_current_root_paddr = 0U;
    }
}

bool AddressSpaceBackend::clone_user_space(uintptr_t src_root, uintptr_t dst_root) noexcept {
    if (src_root == 0U || dst_root == 0U) {
        return false;
    }

    auto& tables = address_space_tables();
    const auto src_iter = tables.find(src_root);
    auto dst_iter = tables.find(dst_root);
    if (src_iter == tables.end() || dst_iter == tables.end()) {
        return false;
    }

    const size_t page_size = get_platform_info().page_size;

    for (const auto& kv : src_iter->second.pages) {
        const uintptr_t vaddr = kv.first;
        const PageMapping& src_mapping = kv.second;

        const uintptr_t dst_page_paddr = alloc_frame();
        if (dst_page_paddr == 0U) {
            return false;
        }

        const auto* src_kva =
            reinterpret_cast<const uint8_t*>(Kernel::MM::phys_to_virt(src_mapping.paddr));
        auto* dst_kva = reinterpret_cast<uint8_t*>(Kernel::MM::phys_to_virt(dst_page_paddr));
        std::memcpy(dst_kva, src_kva, page_size);

        dst_iter->second.pages[vaddr] =
            PageMapping{.paddr = dst_page_paddr, .flags = src_mapping.flags};
    }

    return true;
}

} // namespace HAL

namespace HostHAL {

void reset() noexcept {
    address_space_tables().clear();
    g_current_root_paddr = 0U;
    g_synthetic_root_counter = 1U;
    host_cpu().reset_counters();
    host_mmu().reset();
    host_console().clear();
}

void* phys_to_host_ptr(uintptr_t paddr) noexcept {
    if (paddr < SIMULATED_RAM_BASE || paddr >= SIMULATED_RAM_BASE + SIMULATED_RAM_SIZE) {
        return nullptr;
    }
    return reinterpret_cast<void*>(Kernel::MM::phys_to_virt(paddr));
}

uintptr_t host_ptr_to_phys(const void* ptr) noexcept {
    if (ptr == nullptr) {
        return 0U;
    }
    const auto vaddr = reinterpret_cast<uintptr_t>(ptr);
    const auto base_vaddr = reinterpret_cast<uintptr_t>(get_ram_backing().base());
    if (vaddr < base_vaddr || vaddr >= base_vaddr + SIMULATED_RAM_SIZE) {
        return 0U;
    }
    return Kernel::MM::virt_to_phys(vaddr);
}

const std::string& console_output() noexcept {
    return host_console().output();
}

void clear_console_output() noexcept {
    host_console().clear();
}

size_t dcache_flush_count() noexcept {
    return host_cpu().dcache_flushes();
}

size_t icache_invalidate_count() noexcept {
    return host_cpu().icache_invalidations();
}

size_t active_address_space_count() noexcept {
    return address_space_tables().size();
}

} // namespace HostHAL