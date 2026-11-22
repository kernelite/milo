# ADR 0001: Host & Freestanding C++20 Unit Testing Framework (`snitch`)

## Status
Accepted

## Context
As hardware-independent kernel modules (`Elf64View`, `CpioArchive`, `FDT`, `UserStackBuilder`, `pfa`, and upcoming IPC/scheduler primitives) are decoupled from AArch64 assembly and executed natively on the host via `make test-host`, ad-hoc `assert()` macros and interactive shell commands (`cmd_test_*`) are insufficient for long-term regression testing:

- `assert()` aborts on the first failure without decomposing left-hand/right-hand operands or isolating subcases.
- Frameworks that allocate heap memory internally (`Catch2`, `GoogleTest`) pollute allocator metrics (`pfa`, slab/heap tests) and introduce noise under `AddressSanitizer` (`ASan`).
- Heavy C++ runtime dependencies (`RTTI`, exceptions, CMake/Python generators) conflict with microkernel compiler flags (`-std=c++20 -fno-exceptions -fno-rtti`) and the root `Makefile` build pipeline.

## Decision
We standardize on **`snitch`** (C++20, single-header distribution) for host unit testing (`tests/host/`) and future bare-metal target test runners.

### Key Architectural Conventions
1. **Vendoring & Single-TU Compilation**:
   - The single amalgamated header is stored at `third_party/snitch/include/snitch/snitch_all.hpp` (bootstrapped via `make setup-snitch`).
   - `tests/host/test_main.cpp` defines `#define SNITCH_IMPLEMENTATION` once so test suites (`*_test.cpp`) recompile incrementally in milliseconds.
2. **Compiler & Sanitizer Baseline**:
   - Host tests compile with `-std=c++20 -fno-exceptions -fno-rtti -DSNITCH_WITH_EXCEPTIONS=0 -Wall -Wextra -Werror -Wpedantic -fsanitize=address,undefined`.
   - `kernel/runtime/cxx_abi.cpp` is excluded from host test builds so host `ASan` intercepts all memory accesses and guarantees zero hidden heap allocations.
3. **HAL Mocking Strategy (`tests/host/mocks/`)**:
   - Hardware interfaces (`HAL::Console`, `HAL::CpuControl`, `HAL::MemoryControl`, `HAL::AddressSpaceBackend`) are implemented as deterministic, static-storage mocks in `tests/host/mocks/host_hal.cpp`.
   - `HostTest::reset_host_hal()` resets simulated physical RAM (`16 MiB` page-aligned arena mapped via `HAL::PlatformInfo::higher_half_offset`), page-table entries, and failure-injection hooks at the start of every `TEST_CASE`.
4. **Coexistence with On-Target QEMU Suites**:
   - Hardware-dependent tests (`test el0`, `test svc`, `test abort`, `test uxn_fault`, `test cow`, `test demand_paging`) remain in `kernel/shell/shell.cpp` and `arch/aarch64-linux-gnu/arch_test.cpp` for QEMU verification.
   - Pure logic and data-structure modules live in `tests/host/*_test.cpp` and execute automatically during `make quality`.

## Consequences
- **Positive**: Zero heap allocations inside the test runner; exact operand decomposition on failure; native `-fno-exceptions -fno-rtti` support; clean `ASan`/`UBSan` runs; zero external build-system dependencies.
- **Negative**: Requires C++20 (already enforced across `milo`) and thin hand-written HAL mocks in `tests/host/mocks/` for subsystems touching MMU or CPU interfaces.