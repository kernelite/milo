ARCH ?= aarch64-linux-gnu
TARGET := kernel.bin

# Toolchain targeting bare-metal
PREFIX  := $(ARCH)-
CXX     := $(PREFIX)g++
AS      := $(PREFIX)as
LD      := $(PREFIX)ld
OBJCOPY := $(PREFIX)objcopy

BUILD_DIR          := build
INITRDS_DIR        := initrds
BUSYBOX_ROOTFS_DIR := $(BUILD_DIR)/rootfs-busybox
BASH_ROOTFS_DIR    := $(BUILD_DIR)/rootfs-bash
BUSYBOX_CPIO       := $(INITRDS_DIR)/busybox.cpio
BASH_CPIO          := $(INITRDS_DIR)/bash.cpio
INITRD             ?= $(BUSYBOX_CPIO)

# Freestanding C++ flags
CXXFLAGS := -std=c++20 -ffreestanding -O2 -Wall -Wextra -fno-use-cxa-atexit \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -MMD -MP -g
ASFLAGS  := -g
LDFLAGS  := -nostdlib -T arch/$(ARCH)/linker.ld

# Source lists appended by arch.mk and kernel.mk
SRCS_CXX :=
SRCS_ASM :=
SRCS_C   := user/hello_musl.c

# Include architecture and core modules
include arch/$(ARCH)/arch.mk
include kernel/kernel.mk

# Map sources to build output objects
OBJS := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(SRCS_CXX)) \
        $(patsubst %.s, $(BUILD_DIR)/%.o, $(SRCS_ASM))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean run debug gdb lint format format-check lint-report quality compile_commands.json ai-dump setup-musl setup-busybox setup-bash

all: $(BUILD_DIR)/$(TARGET) $(INITRD)

arch/memory_config.hpp: arch/$(ARCH)/memory_config.hpp
	@mkdir -p arch
	cp $< $@

# Link as ELF executable first
$(BUILD_DIR)/kernel.elf: $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^

# Extract flat binary image with objcopy for QEMU Linux bootloader
$(BUILD_DIR)/$(TARGET): $(BUILD_DIR)/kernel.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/%.o: %.cpp arch/memory_config.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Ikernel -Iarch/$(ARCH) -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

MUSL_CC ?= $(shell command -v aarch64-linux-musl-gcc 2>/dev/null || echo /opt/aarch64-musl/bin/musl-gcc)
LINUX_VER := 6.6.63

setup-musl:
	@curl -fL https://musl.libc.org/releases/musl-1.2.5.tar.gz | tar -xz -C /tmp
	@cd /tmp/musl-1.2.5 && CC=$(PREFIX)gcc CROSS_COMPILE=$(PREFIX) ./configure --prefix=/opt/aarch64-musl --target=aarch64-linux-gnu && $(MAKE) -j$$(nproc) && sudo $(MAKE) install
	@curl -fL https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$(LINUX_VER).tar.xz | tar -xJ -C /tmp
	@cd /tmp/linux-$(LINUX_VER) && sudo $(MAKE) ARCH=arm64 INSTALL_HDR_PATH=/opt/aarch64-musl headers_install
	@sudo ln -sf /opt/aarch64-musl/bin/musl-gcc /usr/local/bin/aarch64-linux-musl-gcc
	@for tool in ar ld nm objcopy objdump ranlib strip; do \
		sudo ln -sf /usr/bin/$(PREFIX)$$tool /usr/local/bin/aarch64-linux-musl-$$tool; \
	done
	sudo rm -rf /tmp/musl-1.2.5 /tmp/linux-$(LINUX_VER)

# Base EL0 payloads compiled once into $(BUILD_DIR)/user/
$(BUILD_DIR)/user/hello: user/hello.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $(BUILD_DIR)/user/hello.o
	$(LD) -nostdlib -z max-page-size=0x1000 -e _start -Ttext=0x00400000 -o $@ $(BUILD_DIR)/user/hello.o

$(BUILD_DIR)/user/hello_musl: user/hello_musl.c
	@mkdir -p $(dir $@)
	$(MUSL_CC) -static -O2 -Wall -Wextra $< -o $@

# --- BusyBox Initrd (initrds/busybox.cpio) ---
BUSYBOX_VER := 1.36.1
BUSYBOX_BIN := $(BUILD_DIR)/busybox-$(BUSYBOX_VER)/busybox

$(BUSYBOX_BIN):
	@mkdir -p $(BUILD_DIR)
	@if [ ! -d "$(BUILD_DIR)/busybox-$(BUSYBOX_VER)" ]; then \
		curl -fL https://busybox.net/downloads/busybox-$(BUSYBOX_VER).tar.bz2 | tar -xj -C $(BUILD_DIR); \
	fi
	@cd $(BUILD_DIR)/busybox-$(BUSYBOX_VER) && \
		$(MAKE) defconfig && \
		sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config && \
		sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' .config && \
		$(MAKE) -j$$(nproc) CROSS_COMPILE=aarch64-linux-musl- CC="$(MUSL_CC)"

$(BUSYBOX_CPIO): $(BUILD_DIR)/user/hello $(BUILD_DIR)/user/hello_musl $(wildcard $(BUSYBOX_BIN))
	@rm -rf $(BUSYBOX_ROOTFS_DIR)
	@mkdir -p $(INITRDS_DIR) $(BUSYBOX_ROOTFS_DIR)/bin $(BUSYBOX_ROOTFS_DIR)/dev $(BUSYBOX_ROOTFS_DIR)/proc $(BUSYBOX_ROOTFS_DIR)/tmp
	@echo "milo-initrd-v1" > $(BUSYBOX_ROOTFS_DIR)/init.txt
	@install -m 0755 $(BUILD_DIR)/user/hello $(BUSYBOX_ROOTFS_DIR)/bin/hello
	@install -m 0755 $(BUILD_DIR)/user/hello_musl $(BUSYBOX_ROOTFS_DIR)/bin/hello_musl
	@if [ -f "$(BUSYBOX_BIN)" ]; then \
		install -m 0755 $(BUSYBOX_BIN) $(BUSYBOX_ROOTFS_DIR)/bin/busybox && \
		ln -sf busybox $(BUSYBOX_ROOTFS_DIR)/bin/sh && \
		for app in ls cat echo pwd ps uname grep wc head tail mkdir rm cp mv sleep true false; do \
			ln -sf busybox $(BUSYBOX_ROOTFS_DIR)/bin/$$app; \
		done; \
	fi
	@cd $(BUSYBOX_ROOTFS_DIR) && find . | cpio -o -H newc > ../../$(BUSYBOX_CPIO) 2>/dev/null

setup-busybox: $(BUSYBOX_BIN)
	@$(MAKE) $(BUSYBOX_CPIO)

# --- GNU Bash Initrd (initrds/bash.cpio) ---
BASH_VER := 5.2.37
BASH_DIR := $(BUILD_DIR)/bash-$(BASH_VER)
BASH_BIN := $(BASH_DIR)/bash

$(BASH_BIN):
	@mkdir -p $(BUILD_DIR)
	@if [ ! -d "$(BASH_DIR)" ]; then \
		curl -fL https://ftp.gnu.org/gnu/bash/bash-$(BASH_VER).tar.gz | tar -xz -C $(BUILD_DIR); \
	fi
	@cd $(BASH_DIR) && \
		CC="$(MUSL_CC)" \
		CFLAGS="-Os -static" \
		LDFLAGS="-static" \
		./configure \
			--host=aarch64-linux-musl \
			--enable-static-link \
			--without-bash-malloc \
			--disable-nls \
			--enable-readline \
			--enable-job-control \
			--enable-process-substitution && \
		$(MAKE) -j$$(nproc) && \
		aarch64-linux-musl-strip bash

$(BASH_CPIO): $(BUILD_DIR)/user/hello $(BUILD_DIR)/user/hello_musl $(BASH_BIN)
	@rm -rf $(BASH_ROOTFS_DIR)
	@mkdir -p $(INITRDS_DIR) $(BASH_ROOTFS_DIR)/bin $(BASH_ROOTFS_DIR)/dev $(BASH_ROOTFS_DIR)/proc $(BASH_ROOTFS_DIR)/tmp
	@echo "milo-initrd-v1" > $(BASH_ROOTFS_DIR)/init.txt
	@install -m 0755 $(BUILD_DIR)/user/hello $(BASH_ROOTFS_DIR)/bin/hello
	@install -m 0755 $(BUILD_DIR)/user/hello_musl $(BASH_ROOTFS_DIR)/bin/hello_musl
	@install -m 0755 $(BASH_BIN) $(BASH_ROOTFS_DIR)/bin/bash
	@ln -sf bash $(BASH_ROOTFS_DIR)/bin/sh
	@cd $(BASH_ROOTFS_DIR) && find . | cpio -o -H newc > ../../$(BASH_CPIO) 2>/dev/null

setup-bash: $(BASH_CPIO)

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR) arch/memory_config.hpp

# QEMU Executable & Flags
QEMU      ?= qemu-system-aarch64
QEMUFLAGS := -M virt -cpu cortex-a53 -m 256M -nographic -kernel $(BUILD_DIR)/$(TARGET) -initrd $(INITRD)
GDB       ?= gdb-multiarch

run: $(BUILD_DIR)/$(TARGET) $(INITRD)
	$(QEMU) $(QEMUFLAGS)

debug: $(BUILD_DIR)/$(TARGET) $(INITRD)
	@echo "Starting QEMU in debug mode. Connect with: make gdb"
	$(QEMU) $(QEMUFLAGS) -s -S

# Attach GDB using the ELF symbol file
gdb: $(BUILD_DIR)/kernel.elf
	$(GDB) $(BUILD_DIR)/kernel.elf -ex "target remote localhost:1234" -ex "layout split"

# Find all header files for formatting/linting targets
HDRS := $(shell find kernel arch -name '*.hpp' -o -name '*.h')

# --- Compilation Database Generation ---
compile_commands.json: clean
	@echo "[BEAR] Generating compilation database..."
	bear -- $(MAKE) all

# --- Code Quality Targets ---
lint: compile_commands.json
	@echo "[LINT] Running clang-tidy on C++ sources..."
	clang-tidy -p . $(SRCS_CXX)

format:
	@echo "[FORMAT] Formatting sources with clang-format..."
	clang-format -i $(SRCS_CXX) $(SRCS_C) $(HDRS)

format-check:
	@echo "[FORMAT-CHECK] Checking source formatting..."
	clang-format --dry-run --Werror $(SRCS_CXX) $(SRCS_C) $(HDRS)

lint-report: compile_commands.json
	@python3 scripts/generate_quality_report.py

quality: format-check lint-report

commit: quality
	git commit

ai-dump:
	./concat_files.sh dump-for-ai.txt arch/ build/ kernel/ Makefile compile_commands.json