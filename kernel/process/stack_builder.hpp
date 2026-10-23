#pragma once

#include "hal/mmu.hpp"
#include "loader/elf64.hpp"
#include "loader/elf_loader.hpp"
#include "mm/address_space.hpp"

#include <cstddef>
#include <cstdint>

namespace Kernel::Process {

constexpr uintptr_t DEFAULT_USER_STACK_TOP = 0x000000003FFF0000ULL;
constexpr size_t DEFAULT_USER_STACK_SIZE = 64U * 1024U; // 64 KiB (16 pages)

struct UserStack {
    uintptr_t phys_base{0U};
    size_t size{0U};
    size_t num_pages{0U};
    uintptr_t virt_top{0U};
    uintptr_t virt_base{0U};
    uintptr_t final_sp_el0{0U};
    uintptr_t argv_vaddr{0U};
    uint64_t argc{0U};
};

enum class StackBuilderError {
    Success = 0,
    AllocationFailed,
    MappingFailed,
    BufferOverflow,
    InvalidAlignment,
    NullArgument
};

const char* stack_builder_error_str(StackBuilderError error) noexcept;

class UserStackBuilder {
  public:
    static Result<UserStack, StackBuilderError>
    create_and_map(MM::AddressSpace& address_space, uintptr_t virt_top = DEFAULT_USER_STACK_TOP,
                   size_t stack_size = DEFAULT_USER_STACK_SIZE) noexcept;

    static Result<uintptr_t, StackBuilderError> build_abi_layout(UserStack& stack, int argc,
                                                                 const char* const* argv,
                                                                 const char* const* envp = nullptr,
                                                                 const Elf64_auxv_t* auxv = nullptr,
                                                                 size_t auxv_count = 0U) noexcept;
};

} // namespace Kernel::Process