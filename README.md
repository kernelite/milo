# 🛡️ Milo — Bare-Metal C++20 Microkernel

[![Build & Test](https://github.com/kernelite/milo/actions/workflows/ci.yml/badge.svg)](https://github.com/kernelite/milo/actions)
![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white)
![Architecture](https://img.shields.io/badge/Arch-AArch64%20%2F%20Arm64-0091BD?style=flat-square&logo=arm&logoColor=white)
![Target Platform](https://img.shields.io/badge/Target-QEMU%20virt%20(Cortex--A53)-FF6600?style=flat-square&logo=qemu&logoColor=white)
![Runtime Environment](https://img.shields.io/badge/Runtime-Freestanding-444444?style=flat-square)
![License](https://img.shields.io/badge/License-GPLv3-blue.svg?style=flat-square)

**Milo** is a modular, freestanding C++20 microkernel with a portable Hardware Abstraction Layer (HAL) and a Linux AArch64 syscall ABI compatibility layer. **AArch64 (Arm64)** on QEMU `virt` (Cortex-A53) serves as the primary reference target.

The kernel enforces strict C++20 encapsulation, RAII resource ownership, and zero-exception/zero-RTTI freestanding discipline across physical/virtual memory management, preemptive multitasking, POSIX-style signals, VFS/IPC, and ELF64 user-space execution.

---

## ⚡ Features

* **AArch64 Reference Target & Portable HAL (`arch/`, `kernel/hal/`):**
  * Exception Level drop sequence (`EL3 → EL2 → EL1`), `VBAR_EL1` vector table dispatch, and FP/SIMD enablement.
  * 4-level AArch64 MMU page tables with per-process user address spaces and Copy-on-Write (CoW) page fault handling.
  * ARM Generic Timer (100 Hz preemptive tick) and PL011 MMIO UART console driver.
* **Freestanding C++20 & Synchronization (`kernel/runtime/`, `kernel/sync/`):**
  * Compiled with `-std=c++20 -ffreestanding -fno-exceptions -fno-rtti` and strict warning/static-analysis gates (`clang-format`, `clang-tidy`).
  * `Kernel::Sync::IrqGuard`: zero-overhead RAII interrupt-masking guard.
* **Memory Management (`kernel/mm/`):**
  * `MM::Page` & `MM::PageFrameAllocator`: FDT-driven physical memory discovery (with `/memreserve/` carveouts) and atomic reference counting for CoW sharing.
  * `MM::KernelHeap`: 8-class slab allocator (16 B – 2048 B) paired with a compound multi-page allocator backing `kmalloc`/`kzalloc`/`krealloc`/`kfree` and `operator new`/`delete`.
  * `MM::AddressSpace`: isolated user page tables, `clone()`, and validated `copy_from_user`/`copy_to_user` transfers.
* **Processes, Scheduling & Signals (`kernel/process/`):**
  * Encapsulated `Process` class with preemptive round-robin `Scheduler` and intrusive `WaitQueue`s.
  * `FdTable` & `FileDescriptor`: RAII file descriptor table with geometric growth bounded by `RLIMIT_NOFILE` and `O_CLOEXEC` support.
  * `SignalTable`: per-process signal masks, `SigAction` handlers, unmaskable `SIGKILL`/`SIGSTOP` invariant enforcement, and EL0 `rt_sigframe` delivery/unwinding (`rt_sigreturn`).
  * `UserStackBuilder`: System V AArch64 ABI initial stack layout (`argc`, `argv`, `envp`, `auxv`).
* **Linux AArch64 ABI & Syscall Layer (`kernel/uapi/`, `kernel/syscalls/`, `kernel/time/`):**
  * Dedicated C-compatible `kernel/uapi/` headers verified at compile time with `sizeof`, `offsetof`, `std::is_trivial_v`, and `std::is_standard_layout_v` `static_assert`s.
  * `Time::Duration` domain wrapper isolating internal timer/nanosecond logic from `linux_timespec`/`linux_timeval`.
  * Modular syscall handlers covering process lifecycle (`clone`, `execve` with `#!` shebangs, `wait4`, `exit`, job control, `prlimit64`), memory (`brk`, `mmap`, `munmap`, `mprotect`), VFS/IO (`openat`, `read`, `write`, `writev`, `pipe2`, `dup`/`dup3`, `fcntl`, `ioctl`, `getdents64`, `ppoll`, `pselect6`), signals (`rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `kill`), and time/system info (`clock_gettime`, `nanosleep`, `times`, `getrusage`, `sysinfo`, `getrandom`, `uname`).
* **VFS, IPC, ELF Loader & Drivers (`kernel/fs/`, `kernel/ipc/`, `kernel/loader/`, `kernel/drivers/`):**
  * Virtual File System with `tmpfs`, `cpio` initrd loading, and `devfs` (interactive TTY with `termios`/`winsize` ioctls and foreground PGID job control).
  * Encapsulated `Kernel::IPC::Pipe` ring buffer with blocking/non-blocking I/O and `poll()` support.
  * Static ELF64 binary loader and VirtIO block device driver (`virtio_blk`).
* **Dual-Target Testing (`tests/host/`, `kernel/shell/`):**
  * Host-native Catch2 unit tests (`HOST_TEST`) for PFA, heap, stack builder, VFS/tmpfs/pipes, and VirtIO rings.
  * Interactive in-kernel shell (`milo>`) with hardware diagnostics and EL0/syscall/scheduler verification suites.

---

## 📁 Repository Structure

```text
.
├── arch/
│   └── aarch64-linux-gnu/       # AArch64 boot, vectors, MMU/CoW, trap handling, PL011 UART, linker script
├── kernel/
│   ├── boot/                    # Flattened Device Tree (FDT) parser & memory map discovery
│   ├── drivers/                 # In-tree hardware drivers (e.g., VirtIO block)
│   ├── fs/                      # VFS core, tmpfs, devfs (TTY), and cpio initrd
│   ├── hal/                     # Architecture-agnostic HAL traits (CPU, MMU, Console, Timer, Platform)
│   ├── ipc/                     # Encapsulated Pipe implementation (Pipe.hpp / Pipe.cpp)
│   ├── loader/                  # ELF64 executable loader
│   ├── mm/                      # Page, PageFrameAllocator, KernelHeap, AddressSpace
│   ├── process/                 # Process, FdTable, FileDescriptor, SignalTable, Scheduler, WaitQueue, StackBuilder
│   ├── runtime/                 # Freestanding C++ ABI support (cxx_abi.cpp)
│   ├── shell/                   # Interactive kernel shell (milo>) & runtime test suite
│   ├── sync/                    # RAII synchronization primitives (IrqGuard.hpp)
│   ├── syscalls/                # Modular Linux AArch64 syscall handlers & dispatcher
│   ├── time/                    # Time::Duration domain wrapper
│   ├── uapi/                    # Compile-time verified Linux AArch64 UAPI structs & constants
│   ├── kernel.mk                # Kernel build rules
│   └── main.cpp                 # Kernel entry point (kmain)
├── tests/
│   └── host/                    # Host-compiled Catch2 unit tests
├── LICENSE                      # GNU General Public License v3.0
└── Makefile                     # Top-level build, QEMU runner, host tests, and quality gates
```

---

## 🛠️ Prerequisites & Toolchain

To build, test, and run Milo locally:

* **Cross Compiler:** `aarch64-linux-gnu-g++`, `aarch64-linux-gnu-as`, `aarch64-linux-gnu-ld`
* **Host Compiler (for `HOST_TEST`):** `g++` (C++20)
* **Emulator:** `qemu-system-aarch64`
* **Quality Tools (Optional):** `clang-format`, `clang-tidy`
* **Debugger (Optional):** `gdb-multiarch`

The `aarch64-linux-gnu` triplet is used to match the Linux AArch64 user-space ABI/syscall convention while compiling bare-metal freestanding code.

---

## 🚀 Building, Testing & Running

### Build All Targets (Kernel + Host Tests)
```bash
make all
```

### Launch in QEMU
Boots `build/kernel.bin` on QEMU `virt` (`cortex-a53`):
```bash
make run
```
To exit QEMU, press `Ctrl+A` then `X`.

### Code Formatting & Static Analysis
```bash
make format    # Format codebase with clang-format
make quality   # Verify formatting and run clang-tidy checks
```

### Debugging with GDB
1. Launch QEMU listening for GDB on port `1234`:
```bash
make debug
```
2. Attach GDB in another terminal:
```bash
make gdb
```

### Clean Build Artifacts
```bash
make clean
```

---

## 💻 Interactive Kernel Shell (`milo>`)

Once booted, the `milo>` shell provides built-in diagnostics and verification commands, including:

* `help` / `info` / `clear` / `halt` — System info (EL, platform, UART), terminal control, and CPU halt (`wfi`).
* `test cpu`, `test mmu`, `test el0`, `test cpp`, `test svc`, `test abort` — Core HAL, exception vector, and EL0 trap checks.
* Subsystem stress & verification suites for physical frame refcounting, slab/compound heap allocations, dynamic `FdTable`/pipe scaling, VFS, ELF loading, `clone`/`execve`/`wait4`, signals, and TTY `ioctl`s.
* `test all` — Run the full integrated in-kernel verification suite.

---

## 🗺️ Roadmap

* **Decoupled Driver Development Kit (DDK):** Introduce public `include/ddk/` interfaces (`Driver`, `PlatformServices`, and RAII `MmioRegion`/`DmaBuffer`/`IrqHandle`) and a top-level `drivers/` tree isolated from private `kernel/` headers, with out-of-tree build support.
* **Additional Drivers & Storage:** Migrate `virtio_blk` to the DDK, add persistent filesystem support, and expand peripheral/networking drivers.
* **Multi-Architecture Expansion:** Implement HAL backends for additional targets (e.g., `x86_64`, `riscv64`) under `arch/`.

---

## 📜 License

Distributed under the **GNU General Public License v3.0 (GPL-3.0)**. See [`LICENSE`](LICENSE) for details.
````[cite: 1]
