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
