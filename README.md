# Bare-Metal C++ Microkernel

A modular, freestanding C++20 microkernel designed with a portable Hardware Abstraction Layer (HAL) for multi-architecture support. **AArch64 (Arm64)** serves as the initial reference implementation (targeting QEMU `virt` / ARM Cortex-A53), with expansion planned for additional architectures in future releases.

The project demonstrates low-level hardware initialization, Exception Level drops (EL3 → EL2 → EL1), interactive user-mode (EL0) context switching via `eret` and `svc` trap handling, an abstract HAL interface, and an interactive kernel shell.

---

## Features

* **Multi-Architecture Ready Design:** Strict separation between architecture-agnostic kernel interfaces (`kernel/hal/`) and architecture-specific implementations (`arch/`), allowing seamless porting to new target architectures.
* **AArch64 Reference Target (Primary):**
* Multilevel Exception Level drop setup (EL3 → EL2 → EL1).
* `VBAR_EL1` vector table registration and exception trap dispatch.
* FP/SIMD coprocessor activation in `CPACR_EL1`.


* **Freestanding C++20 Runtime:** Built with `-ffreestanding`, `-fno-exceptions`, and `-fno-rtti`. Includes custom freestanding ABI handlers (`cxx_abi.cpp`) and safe compile-time static driver initialization via `constinit`.
* **Hardware Abstraction Layer (HAL):**
* **Console HAL:** Trait-based console interface with PL011 MMIO UART driver implementation (`0x09000000`).
* **CPU HAL:** Target-agnostic CPU state queries, interrupt masking, low-power WFI state execution.
* **MMU HAL:** SCTLR_EL1 system register status inspection (MMU, I-Cache, D-Cache).


* **EL0 User-Mode Context Switching:**
* Context save/restore routines for switching between EL1 kernel and EL0 user mode.
* Exception trap handling for Linux AArch64 `SVC #0` syscalls (`sys_write`, `sys_exit`).


* **Interactive Kernel Shell & Verification Suite:** Built-in command line interface (`milo>`) featuring hardware diagnostics and runtime unit testing.

---

## Repository Structure

```text
.
├── arch/
│   └── aarch64-linux-gnu/       # Initial AArch64 target implementation
│       ├── arch.mk              # Architecture-specific Makefile rules
│       ├── boot.s               # Low-level boot code, vector tables, EL transitions
│       ├── cpu_aarch64.cpp      # AArch64 HAL CPU control implementation
│       ├── cpu_context.hpp      # Architectural register definitions & assembly helpers
│       ├── linker.ld            # Linker script (entry point at 0x40000000)
│       ├── mmu_aarch64.hpp      # AArch64 HAL MMU control implementation
│       └── uart_console.cpp     # PL011 UART console driver
├── kernel/
│   ├── core/
│   │   └── syscall_dispatcher.cpp  # Linux syscall trap dispatcher
│   ├── hal/                     # Architecture-agnostic Hardware Abstraction Layer
│   │   ├── console.hpp          # Abstract console trait
│   │   ├── cpu.hpp              # Abstract CPU state & context trait
│   │   ├── mmu.hpp              # Abstract MMU & memory trait
│   │   ├── platform.hpp         # Platform initialization headers
│   │   └── trap.hpp             # Abstract trap frame and handler interfaces
│   ├── runtime/
│   │   └── cxx_abi.cpp          # C++ freestanding ABI stubs (__cxa_atexit, operator delete)
│   ├── shell/
│   │   ├── shell.hpp            # Interactive shell interface
│   │   └── shell.cpp            # Kernel shell & integrated test suite implementation
│   ├── kernel.mk                # Kernel module Makefile definitions
│   └── main.cpp                 # Kernel entry point (kmain)
├── concat_files.sh              # Project file consolidation utility
├── LICENSE                      # GNU General Public License v3.0
└── Makefile                     # Build system and QEMU workflow runner
```

---

## Architecture Portability & Expansion Plans

The codebase enforces a clean boundary between high-level kernel logic and platform hardware via abstract C++ interfaces in `kernel/hal/`.

* **Phase 1 (Current):** **AArch64 (Arm64)** reference implementation (`arch/aarch64-linux-gnu/`).
* **Phase 2 (Planned):** Support for additional OS features: scheduler, multiuser, security, hardware drivers, etc...
* **Phase 3 (Planned):** Support for additional CPU architectures (such as **x86_64** or **RISC-V**) by creating new subdirectories under `arch/` and implementing the required HAL traits (`HAL::CpuControl`, `HAL::MemoryControl`, `HAL::Console`).

---

## Prerequisites & Toolchain

To build and run the reference AArch64 kernel image locally:

* **Cross Compiler:** `aarch64-linux-gnu-g++`, `aarch64-linux-gnu-as`, `aarch64-linux-gnu-ld`
* **Emulator:** `qemu-system-aarch64`
* **Debugger (Optional):** `gdb-multiarch`

---

## Building and Running

### Build the Kernel Image

To compile C++ and Assembly sources and generate `build/kernel.bin` (defaults to `ARCH=aarch64-linux-gnu`):
```bash
make
```

### Launch in QEMU

Run the compiled image inside QEMU (`virt` machine, `cortex-a53` CPU):
```bash
make run
```

To exit QEMU, press `Ctrl+A` then `X`.

### Debugging with GDB

1. Launch QEMU in listening mode (gdbserver on port `1234`):
```bash
make debug
```

2. In a separate terminal, attach GDB:
```bash
make gdb
```

### Cleaning Build Artifacts
```bash
make clean
```

---

## Interactive Kernel Shell (`milo>`)

Once booted, the shell provides the following built-in diagnostic and test commands:

| Command | Description |
| --- | --- |
| `help` | Display command usage menu |
| `info` | Display current Exception Level (EL), hardware model, and UART driver info |
| `clear` | Clear terminal using VT100 escape sequences |
| `test cpu` | Test interrupt enable/disable masking via HAL |
| `test mmu` | Inspect SCTLR_EL1 register to report MMU and Cache statuses |
| `test el0` | Execute context switch into EL0 user space and verify `SVC #0` trap return |
| `test cpp` | Verify `.bss` zero-initialization and C++ vtable dynamic dispatch |
| `test all` | Run full integrated verification test suite |
| `halt` | Issue `wfi` (Wait for Interrupt) to halt CPU |

---

## AArch64 Execution Flow

### Exception Level Sequence

1. **Entry (`_start` in `boot.s`):** Queries `CurrentEL`.
2. **EL3/EL2 Setup:** Configures `SCR_EL3`, `HCR_EL2`, and drops execution down to **EL1** via `eret`.
3. **EL1 Core Setup:** Registers `vbar_el1` to point to `el1_vector_table`, initializes kernel boot stack (`_boot_stack_top`), enables SIMD/FP via `cpacr_el1`, and branches to `kmain()`.

### EL0 User Space Transition

* The kernel invokes `enter_user_mode()` to save kernel context (`kernel_ctx`) and transition to EL0 via `eret`.
* Code running in EL0 executes instructions and triggers an exception using `svc #0`.
* The vector table traps the exception in `el0_sync_handler`, evaluates `esr_el1`, routes the syscall through `return_to_kernel`, restores `kernel_ctx`, and cleanly returns control to the shell.

---

## License

Distributed under the **GNU General Public License v3.0 (GPL-3.0)**. See [`LICENSE`](https://www.google.com/search?q=LICENSE) for details.