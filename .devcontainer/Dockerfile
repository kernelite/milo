FROM mcr.microsoft.com/devcontainers/rust:1-bookworm

# Install Arm64 cross-compilation toolchain, QEMU, ROMs, and debugging tools
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \
        gcc-aarch64-linux-gnu \
        g++-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu \
        make \
        qemu-system-aarch64 \
        ipxe-qemu \
        gdb-multiarch \
    && apt-get clean && rm -rf /var/lib/apt/lists/*
