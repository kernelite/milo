FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

ARG MUSL_VER=1.2.5
ARG LINUX_VER=6.6.63
ARG SNITCH_VER=1.3.2

ENV DEBIAN_FRONTEND=noninteractive

# 1. Install AArch64 bare-metal toolchain, CMake, linters, build utilities, and QEMU/GDB
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    tar \
    xz-utils \
    bzip2 \
    gzip \
    cpio \
    python3 \
    git \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    clang \
    clang-format \
    clang-tidy \
    bear \
    qemu-system-arm \
    ipxe-qemu \
    gdb-multiarch \
    && rm -rf /var/lib/apt/lists/*

# 2. Build and install musl-libc 1.2.5 for AArch64 into /opt/aarch64-musl
RUN curl -fL "https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz" | tar -xz -C /tmp \
    && cd "/tmp/musl-${MUSL_VER}" \
    && CC=aarch64-linux-gnu-gcc CROSS_COMPILE=aarch64-linux-gnu- \
       ./configure --prefix=/opt/aarch64-musl --target=aarch64-linux-gnu \
    && make -j"$(nproc)" \
    && make install \
    && rm -rf "/tmp/musl-${MUSL_VER}"

# 3. Install Linux UAPI headers into /opt/aarch64-musl/include
RUN curl -fL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${LINUX_VER}.tar.xz" | tar -xJ -C /tmp \
    && make -C "/tmp/linux-${LINUX_VER}" ARCH=arm64 INSTALL_HDR_PATH=/opt/aarch64-musl headers_install \
    && rm -rf "/tmp/linux-${LINUX_VER}"

# 4. Expose aarch64-linux-musl-* toolchain wrappers on PATH
RUN ln -sf /opt/aarch64-musl/bin/musl-gcc /usr/local/bin/aarch64-linux-musl-gcc \
    && for tool in ar ld nm objcopy objdump ranlib strip; do \
        ln -sf "/usr/bin/aarch64-linux-gnu-${tool}" "/usr/local/bin/aarch64-linux-musl-${tool}"; \
    done

# 5. Build and install snitch C++20 unit testing framework into /usr/local
RUN curl -fL "https://github.com/snitch-org/snitch/archive/refs/tags/v${SNITCH_VER}.tar.gz" | tar -xz -C /tmp \
    && cmake -S "/tmp/snitch-${SNITCH_VER}" -B "/tmp/snitch-${SNITCH_VER}/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++ \
        -DCMAKE_CXX_FLAGS="-fno-exceptions -fno-rtti" \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DSNITCH_WITH_EXCEPTIONS=OFF \
        -DSNITCH_DO_TEST=OFF \
    && cmake --build "/tmp/snitch-${SNITCH_VER}/build" -j"$(nproc)" \
    && cmake --install "/tmp/snitch-${SNITCH_VER}/build" \
    && rm -rf "/tmp/snitch-${SNITCH_VER}"
