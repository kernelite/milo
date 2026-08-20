# Arm64 Rust Microkernel

A modular, bare-metal Arm64 microkernel written in Rust (`no_std`), designed for execution on QEMU's `virt` platform. This project aims to build an isolated, portable microkernel architecture capable of executing POSIX binaries and user shells via a Linux ABI translation layer.

---

## Features

* **Pure Rust Kernel Core:** `#![no_std]` and `#![no_main]` implementation targeting `aarch64-unknown-none`.
* **Hardware Bootstrap:** Assembly initialization (`boot.s`) supporting multi-core parking, stack alignment, BSS zeroing, and `CPACR_EL1` FP/SIMD vector trap enablement.
* **Interactive Shell:** Native PL011 UART driver providing buffered line input, backspace handling, and command execution.
* **100% Containerized Development:** Pre-configured VS Code Devcontainer and GitHub Codespaces support with pre-built cross-compilation toolchains and QEMU emulation.

---

## Project Layout

```text
.
├── .cargo/
│   └── config.toml          # Rust build flags and target features
├── .devcontainer/
│   ├── Dockerfile           # Toolchain, QEMU, and GDB container definition
│   └── devcontainer.json    # VS Code extension and workspace settings
├── src/
│   └── lib.rs               # Kernel entry point (kmain), UART driver, and shell
├── boot.s                   # Arm64 assembly bootstrap
├── linker.ld                # Memory layout linker script
├── Makefile                 # Automated build and run workflow
└── Cargo.toml               # Kernel package specification

```

---

## Getting Started

### Option 1: VS Code Devcontainers / GitHub Codespaces (Recommended)

1. Open this repository in **VS Code** with the **Dev Containers** extension installed, or open it directly in **GitHub Codespaces**.
2. When prompted, click **Reopen in Container**.
3. Once loaded, open a terminal and run:
```bash
make run

```

### Option 2: Local Setup

**Prerequisites:**

* Rust toolchain with `aarch64-unknown-none` target (`rustup target add aarch64-unknown-none`)
* `aarch64-linux-gnu-as`, `aarch64-linux-gnu-ld`
* `qemu-system-aarch64`

**Build and Execution:**

```bash
# Compile and link the kernel image
make

# Launch kernel inside QEMU
make run

# Clean build artifacts
make clean

```

---

## Roadmap to User Space & Bash Execution

* [x] **Phase 0:** Bare-metal Arm64 boot, PL011 UART driver, and interactive shell.
* [ ] **Phase 1: Memory Management**
  * [ ] Physical Frame Allocator (4KB page allocator).
  * [ ] Global Heap Allocator (`alloc` crate support).
  * [ ] 4-Level Page Table management (`TTBR0_EL1` / `TTBR1_EL1`).

* [ ] **Phase 2: Exception Handling & Syscalls**
  * [ ] Exception Vector Table (`VBAR_EL1`).
  * [ ] EL0 User Space Context Switching (`eret`).
  * [ ] `svc #0` Linux AArch64 syscall dispatcher.

* [ ] **Phase 3: Process Execution**
  * [ ] RAMDisk / CPIO initrd parser.
  * [ ] ELF binary loader & user stack setup.

* [ ] **Phase 4: POSIX Abstraction**
  * [ ] Basic TTY/FD syscalls (`sys_read`, `sys_write`).
  * [ ] Process management (`sys_clone`, `sys_execve`, `sys_exit`).

* [ ] **Phase 5: Shell Milestone**
  * [ ] Run static `musl-libc` binaries.
  * [ ] Boot BusyBox `ash` shell / GNU `bash`.

---

## License

Distributed under the GPL License.
