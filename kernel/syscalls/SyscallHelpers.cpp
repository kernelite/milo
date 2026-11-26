#include "hal/cpu.hpp"
#include "hal/targets/qemu_virt/timer.hpp"
#include "mm/pfa.hpp"
#include "process/scheduler.hpp"
#include "syscalls/SyscallInternal.hpp"

namespace Kernel::Syscalls {

void wait_for_event_or_tick() noexcept {
    auto& cpu = HAL::get_cpu();
    Kernel::Scheduler::yield();
    cpu.enable_interrupts();
    cpu.halt();
    cpu.disable_interrupts();
}

Kernel::Time::Duration monotonic_now() noexcept {
    const uint64_t freq = HAL::QemuVirt::GenericTimer::frequency_hz();
    const uint64_t ticks = HAL::QemuVirt::GenericTimer::counter_ticks();
    return Kernel::Time::Duration::from_ticks(ticks, freq);
}

uint64_t monotonic_now_ns() noexcept {
    return monotonic_now().nanoseconds();
}

size_t cstr_len(const char* str) noexcept {
    size_t len = 0U;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

void cstr_copy(char* dst, const char* src, size_t cap) noexcept {
    if (dst == nullptr || cap == 0U) {
        return;
    }
    size_t idx = 0U;
    if (src != nullptr) {
        for (; idx + 1U < cap && src[idx] != '\0'; ++idx) {
            dst[idx] = src[idx];
        }
    }
    dst[idx] = '\0';
}

bool cstr_eq(const char* lhs, const char* rhs) noexcept {
    while (*lhs != '\0' && *lhs == *rhs) {
        lhs++;
        rhs++;
    }
    return *lhs == *rhs;
}

// Normalizes a base directory + relative/absolute input path into a canonical "/a/b" path
bool normalize_vfs_path(const char* base_dir, const char* input, char* out,
                        size_t out_cap) noexcept {
    if (input == nullptr || out == nullptr || out_cap < 2U) {
        return false;
    }

    char combined[Kernel::Process::MAX_PATH_LEN * 2U]{};
    size_t pos = 0U;

    if (input[0] != '/') {
        const char* base = (base_dir != nullptr && base_dir[0] == '/') ? base_dir : "/";
        for (size_t i = 0U; base[i] != '\0'; ++i) {
            if (pos + 2U >= sizeof(combined)) {
                return false;
            }
            combined[pos++] = base[i];
        }
        if (pos > 0U && combined[pos - 1U] != '/') {
            combined[pos++] = '/';
        }
    }

    for (size_t i = 0U; input[i] != '\0'; ++i) {
        if (pos + 1U >= sizeof(combined)) {
            return false;
        }
        combined[pos++] = input[i];
    }
    combined[pos] = '\0';

    size_t comp_starts[32]{};
    size_t comp_count = 0U;
    out[0] = '/';
    size_t out_len = 1U;

    size_t idx = 0U;
    while (combined[idx] != '\0') {
        while (combined[idx] == '/') {
            idx++;
        }
        if (combined[idx] == '\0') {
            break;
        }

        const size_t start = idx;
        while (combined[idx] != '\0' && combined[idx] != '/') {
            idx++;
        }
        const size_t len = idx - start;

        if (len == 1U && combined[start] == '.') {
            continue;
        }
        if (len == 2U && combined[start] == '.' && combined[start + 1U] == '.') {
            if (comp_count > 0U) {
                out_len = comp_starts[--comp_count];
                if (out_len == 0U) {
                    out[0] = '/';
                    out_len = 1U;
                }
            }
            continue;
        }

        if (comp_count >= 32U) {
            return false;
        }
        comp_starts[comp_count++] = out_len;

        if (out_len > 1U) {
            if (out_len + 1U >= out_cap) {
                return false;
            }
            out[out_len++] = '/';
        }
        if (out_len + len >= out_cap) {
            return false;
        }
        for (size_t k = 0U; k < len; ++k) {
            out[out_len++] = combined[start + k];
        }
    }

    out[out_len] = '\0';
    return true;
}

Kernel::FS::File* get_proc_file(Kernel::Process::Process* proc, uint64_t raw_fd) noexcept {
    if (proc == nullptr) {
        return nullptr;
    }
    return proc->fds().get_file(raw_fd);
}

linux_stat inode_to_linux_stat(const Kernel::FS::Inode& inode) noexcept {
    const linux_timespec atime =
        Kernel::Time::Duration::from_nanoseconds(inode.get_atime_ns()).to_linux_timespec();
    const linux_timespec mtime =
        Kernel::Time::Duration::from_nanoseconds(inode.get_mtime_ns()).to_linux_timespec();
    const linux_timespec ctime =
        Kernel::Time::Duration::from_nanoseconds(inode.get_ctime_ns()).to_linux_timespec();

    linux_stat stat_out{};
    stat_out.st_dev = 1ULL;
    stat_out.st_ino = (inode.get_ino() != 0U) ? inode.get_ino() : 1ULL;
    stat_out.st_mode = inode.get_mode();
    stat_out.st_nlink = (inode.get_nlink() != 0U) ? inode.get_nlink() : 1U;
    stat_out.st_uid = inode.get_uid();
    stat_out.st_gid = inode.get_gid();
    stat_out.st_rdev = inode.get_rdev();
    stat_out.st_size = static_cast<int64_t>(inode.get_size());
    stat_out.st_blksize = 4096;
    stat_out.st_blocks = static_cast<int64_t>((inode.get_size() + 511U) / 512U);
    stat_out.st_atime_sec = atime.tv_sec;
    stat_out.st_atime_nsec = atime.tv_nsec;
    stat_out.st_mtime_sec = mtime.tv_sec;
    stat_out.st_mtime_nsec = mtime.tv_nsec;
    stat_out.st_ctime_sec = ctime.tv_sec;
    stat_out.st_ctime_nsec = ctime.tv_nsec;
    return stat_out;
}

void unmap_and_free_range(Kernel::MM::AddressSpace& adrsp, uintptr_t start_vaddr,
                          uintptr_t end_vaddr, size_t page_size) noexcept {
    constexpr uint64_t PTE_DEMAND_ZERO = 1ULL << 56U;
    const uintptr_t page_mask = page_size - 1U;

    for (uintptr_t page_vaddr = start_vaddr; page_vaddr < end_vaddr; page_vaddr += page_size) {
        uintptr_t out_paddr = 0U;
        HAL::PageFlags out_flags = HAL::PageFlags::None;
        if (adrsp.query_user_page(page_vaddr, out_paddr, out_flags)) {
            adrsp.unmap_user_page(page_vaddr);
            Kernel::MM::free_frame(out_paddr & ~page_mask);
        } else {
            uint64_t raw_pte = 0ULL;
            if (adrsp.query_pte_flags(page_vaddr, raw_pte) && (raw_pte & PTE_DEMAND_ZERO) != 0ULL) {
                adrsp.unmap_user_page(page_vaddr);
            }
        }
    }
}

bool validate_user_range(uintptr_t root_paddr, uintptr_t user_buf, size_t count,
                         HAL::PageFlags required_perm) noexcept {
    constexpr uint64_t DESC_COW = 1ULL << 55U;
    constexpr uint64_t PTE_DEMAND_ZERO = 1ULL << 56U;
    constexpr uint64_t PTE_DZ_FLAGS_SHIFT = 8U;
    constexpr uint64_t PTE_DZ_FLAGS_MASK = 0xFFULL << PTE_DZ_FLAGS_SHIFT;

    const uintptr_t va_limit = Kernel::MM::user_addr_space_limit();
    if (user_buf >= va_limit) {
        return false;
    }
    if (count == 0U) {
        return true;
    }
    if (root_paddr == 0U || count > (va_limit - user_buf)) {
        return false;
    }

    const size_t page_size = HAL::get_platform_info().page_size;
    const uintptr_t page_mask = page_size - 1U;
    const uintptr_t first_page = user_buf & ~page_mask;
    const uintptr_t last_page = (user_buf + count - 1U) & ~page_mask;

    for (uintptr_t page_vaddr = first_page; page_vaddr <= last_page; page_vaddr += page_size) {
        uintptr_t out_paddr = 0U;
        HAL::PageFlags flags = HAL::PageFlags::None;
        if (!Kernel::MM::AddressSpace::query_user_page(root_paddr, page_vaddr, out_paddr, flags)) {
            uint64_t raw_pte = 0ULL;
            if (!Kernel::MM::AddressSpace::query_pte_flags(root_paddr, page_vaddr, raw_pte) ||
                (raw_pte & PTE_DEMAND_ZERO) == 0ULL) {
                return false;
            }
            flags =
                static_cast<HAL::PageFlags>((raw_pte & PTE_DZ_FLAGS_MASK) >> PTE_DZ_FLAGS_SHIFT);
        } else if (!HAL::has_flag(flags, HAL::PageFlags::Write)) {
            uint64_t raw_pte = 0ULL;
            if (Kernel::MM::AddressSpace::query_pte_flags(root_paddr, page_vaddr, raw_pte) &&
                (raw_pte & DESC_COW) != 0ULL) {
                flags |= HAL::PageFlags::Write;
            }
        }
        if (!HAL::has_flag(flags, HAL::PageFlags::User) || !HAL::has_flag(flags, required_perm)) {
            return false;
        }
    }

    return true;
}

bool validate_user_read_range(uintptr_t root_paddr, uintptr_t user_buf, size_t count) noexcept {
    return validate_user_range(root_paddr, user_buf, count, HAL::PageFlags::Read);
}

bool validate_user_write_range(uintptr_t root_paddr, uintptr_t user_buf, size_t count) noexcept {
    return validate_user_range(root_paddr, user_buf, count, HAL::PageFlags::Write);
}

int64_t copy_user_cstring(uintptr_t root_paddr, uintptr_t user_str, char* dst, size_t max_len,
                          size_t& out_len) noexcept {
    if (user_str == 0U || dst == nullptr || max_len == 0U) {
        return ERR_EFAULT;
    }

    for (size_t idx = 0U; idx < max_len; ++idx) {
        char chr = '\0';
        const int64_t res = Kernel::MM::copy_from_user(root_paddr, &chr, user_str + idx, 1U);
        if (res < 0) {
            return ERR_EFAULT;
        }
        dst[idx] = chr;
        if (chr == '\0') {
            out_len = idx;
            return 0;
        }
    }

    return ERR_E2BIG;
}

int64_t stage_user_string_array(uintptr_t root_paddr, uintptr_t user_array, const char** out_ptrs,
                                size_t max_entries, char* pool, size_t pool_cap,
                                size_t& pool_offset, int& out_count) noexcept {
    out_count = 0;
    out_ptrs[0] = nullptr;
    if (user_array == 0U) {
        return 0;
    }

    for (size_t idx = 0U; idx <= max_entries; ++idx) {
        uint64_t user_str_ptr = 0ULL;
        const uintptr_t elem_vaddr = user_array + (idx * sizeof(uint64_t));
        const int64_t ptr_res =
            Kernel::MM::copy_from_user(root_paddr, &user_str_ptr, elem_vaddr, sizeof(uint64_t));
        if (ptr_res < 0) {
            return ERR_EFAULT;
        }

        if (user_str_ptr == 0ULL) {
            out_ptrs[idx] = nullptr;
            out_count = static_cast<int>(idx);
            return 0;
        }

        if (idx == max_entries || pool_offset >= pool_cap) {
            return ERR_E2BIG;
        }

        size_t str_len = 0U;
        const int64_t str_res =
            copy_user_cstring(root_paddr, static_cast<uintptr_t>(user_str_ptr), pool + pool_offset,
                              pool_cap - pool_offset, str_len);
        if (str_res < 0) {
            return str_res;
        }

        out_ptrs[idx] = pool + pool_offset;
        pool_offset += str_len + 1U;
    }

    return ERR_E2BIG;
}

int64_t resolve_dirfd_dentry(Kernel::Process::Process& proc, int32_t dirfd,
                             Kernel::FS::Dentry*& out_base) noexcept {
    if (dirfd == AT_FDCWD) {
        return Kernel::FS::vfs_namei(Kernel::FS::StringView(proc.cwd()), nullptr, &out_base);
    }
    Kernel::FS::File* dir_file = get_proc_file(&proc, static_cast<uint64_t>(dirfd));
    if (dir_file == nullptr) {
        return ERR_EBADF;
    }
    if (dir_file->get_inode() == nullptr || !dir_file->get_inode()->is_dir() ||
        dir_file->get_dentry() == nullptr) {
        return ERR_ENOTDIR;
    }
    dir_file->get_dentry()->retain();
    out_base = dir_file->get_dentry();
    return 0;
}

void build_dentry_path(const Kernel::FS::Dentry* target, char* out, size_t cap) noexcept {
    if (out == nullptr || cap < 2U) {
        return;
    }
    if (target == nullptr || target->is_root() || target->get_parent() == nullptr) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    const Kernel::FS::Dentry* chain[32]{};
    size_t depth = 0U;
    for (const Kernel::FS::Dentry* cur = target;
         cur != nullptr && cur->get_parent() != nullptr && depth < 32U; cur = cur->get_parent()) {
        chain[depth++] = cur;
    }
    size_t pos = 0U;
    for (size_t idx = depth; idx > 0U; --idx) {
        const auto name = chain[idx - 1U]->name_view();
        if (pos + 1U + name.size() >= cap) {
            break;
        }
        out[pos++] = '/';
        for (size_t cidx = 0U; cidx < name.size(); ++cidx) {
            out[pos++] = name[cidx];
        }
    }
    if (pos == 0U) {
        out[pos++] = '/';
    }
    out[pos] = '\0';
}

} // namespace Kernel::Syscalls
