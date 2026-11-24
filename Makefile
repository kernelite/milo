ARCH ?= aarch64-linux-gnu
TARGET := kernel.bin
NPROC ?= $(shell nproc 2>/dev/null || echo 4)

# Toolchain targeting bare-metal
PREFIX  := $(ARCH)-
CXX     := $(PREFIX)g++
AS      := $(PREFIX)as
LD      := $(PREFIX)ld
OBJCOPY := $(PREFIX)objcopy

BUILD_DIR          := build
REPORTS_DIR        := $(BUILD_DIR)/reports
INITRDS_DIR        := initrds
BUSYBOX_ROOTFS_DIR := $(BUILD_DIR)/rootfs-busybox
BASH_ROOTFS_DIR    := $(BUILD_DIR)/rootfs-bash
BUSYBOX_CPIO       := $(INITRDS_DIR)/busybox.cpio
BASH_CPIO          := $(INITRDS_DIR)/bash.cpio
INITRD             ?= $(BUSYBOX_CPIO)

# Freestanding C++ flags
CXXFLAGS := -std=c++20 -ffreestanding -fsized-deallocation -O2 -Wall -Wextra -fno-use-cxa-atexit \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -MMD -MP -g
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

# --- Host Unit Testing Toolchain (snitch & gcovr pre-installed in devcontainer) ---
HOST_CXX       ?= g++
HOST_BUILD_DIR := $(BUILD_DIR)/host-tests
HOST_COV_DIR   := $(BUILD_DIR)/host-coverage

HOST_CXXFLAGS := -std=c++20 -O1 -g -Wall -Wextra -Werror -Wpedantic \
                 -fno-exceptions -fno-rtti -DHOST_TEST -DSNITCH_WITH_EXCEPTIONS=0 \
                 -fsanitize=address,undefined -fno-omit-frame-pointer -MMD -MP
HOST_LDFLAGS  := -fsanitize=address,undefined -L/usr/local/lib -lsnitch
HOST_INCLUDES := -Ikernel -Iarch/$(ARCH) -Itests/host

HOST_COV_CXXFLAGS := -std=c++20 -O0 -g --coverage -Wall -Wextra -Werror -Wpedantic \
                     -fno-exceptions -fno-rtti -DHOST_TEST -DSNITCH_WITH_EXCEPTIONS=0 -MMD -MP
HOST_COV_LDFLAGS  := --coverage -L/usr/local/lib -lsnitch

HOST_ONLY_TEST_SRCS := \
    tests/host/mocks/host_hal.cpp \
    tests/host/pfa_test.cpp \
    tests/host/vfs_test.cpp \
    tests/host/rootfs_test.cpp \
    tests/host/tmpfs_test.cpp \
    tests/host/stack_builder_test.cpp \
    tests/host/fdt_test.cpp \
    tests/host/virtio_ring_test.cpp

HOST_KERNEL_SRCS := \
    kernel/boot/fdt.cpp \
    kernel/drivers/irq/gicv2.cpp \
    kernel/drivers/virtio/virtio_ring.cpp \
    kernel/drivers/virtio/virtio_mmio.cpp \
	kernel/drivers/virtio/virtio_blk.cpp \
    kernel/fs/cpio.cpp \
    kernel/fs/vfs.cpp \
    kernel/fs/rootfs/rootfs.cpp \
    kernel/fs/devfs/devfs.cpp \
    kernel/fs/tmpfs/tmpfs.cpp \
    kernel/ipc/pipe.cpp \
    kernel/mm/pfa.cpp \
    kernel/mm/heap.cpp \
    kernel/mm/address_space.cpp \
    kernel/process/process.cpp \
    kernel/process/resource.cpp \
    kernel/process/scheduler.cpp \
    kernel/process/stack_builder.cpp \
    kernel/process/wait_queue.cpp

HOST_TEST_SRCS      := $(HOST_ONLY_TEST_SRCS) $(HOST_KERNEL_SRCS)
HOST_ONLY_TEST_OBJS := $(patsubst %.cpp, $(HOST_BUILD_DIR)/%.o, $(HOST_ONLY_TEST_SRCS))
HOST_TEST_OBJS      := $(patsubst %.cpp, $(HOST_BUILD_DIR)/%.o, $(HOST_TEST_SRCS))
HOST_TEST_DEPS      := $(HOST_TEST_OBJS:.o=.d)
HOST_TEST_BIN       := $(HOST_BUILD_DIR)/kernel_host_tests

HOST_COV_OBJS := $(patsubst %.cpp, $(HOST_COV_DIR)/%.o, $(HOST_TEST_SRCS))
HOST_COV_DEPS := $(HOST_COV_OBJS:.o=.d)
HOST_COV_BIN  := $(HOST_COV_DIR)/kernel_host_coverage

.PHONY: all clean distclean clean-tests run debug gdb lint format format-check lint-report quality \
        test-host compile_commands.json ai-dump setup-musl setup-busybox setup-bash setup-hooks

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

# --- Host Unit Tests (snitch) ---
$(HOST_BUILD_DIR)/%.o: %.cpp arch/memory_config.hpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(HOST_INCLUDES) -c $< -o $@

$(HOST_TEST_BIN): $(HOST_TEST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $^ $(HOST_LDFLAGS) -o $@

# --- Host Test Coverage (gcov instrumentation) ---
$(HOST_COV_DIR)/%.o: %.cpp arch/memory_config.hpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(HOST_COV_CXXFLAGS) $(HOST_INCLUDES) -c $< -o $@

$(HOST_COV_BIN): $(HOST_COV_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $^ $(HOST_COV_LDFLAGS) -o $@

# --- Unified Test Execution & Reporting ---
test-host: $(HOST_TEST_BIN) $(HOST_COV_BIN)
	@mkdir -p $(REPORTS_DIR)
	@echo "[TEST-HOST] Running ASan/UBSan test suite..."
	./$(HOST_TEST_BIN)
	@echo "[COVERAGE] Capturing gcov traces and XML test report..."
	@find $(HOST_COV_DIR) -name '*.gcda' -delete
	./$(HOST_COV_BIN) -r xml -o $(REPORTS_DIR)/test_report.xml
	@echo "[GCOVR] Generating terminal summary, HTML dashboard, and CI XML artifacts..."
	gcovr -j $(NPROC) -r . --object-directory $(HOST_COV_DIR) --filter 'kernel/' \
		--txt \
		--html-details $(REPORTS_DIR)/coverage_report.html \
		--cobertura $(REPORTS_DIR)/cobertura.xml \
		--jacoco $(REPORTS_DIR)/jacoco.xml
	@echo "Test and coverage reports generated in $(REPORTS_DIR)/"

clean-tests:
	rm -rf $(HOST_BUILD_DIR) $(HOST_COV_DIR)

MUSL_CC       ?= $(shell command -v aarch64-linux-musl-gcc 2>/dev/null || echo /opt/aarch64-musl/bin/musl-gcc)
HAS_MUSL_CC   := $(wildcard $(MUSL_CC))
MUSL_USER_BIN := $(if $(HAS_MUSL_CC),$(BUILD_DIR)/user/hello_musl)
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

$(BUSYBOX_CPIO): $(BUILD_DIR)/user/hello $(MUSL_USER_BIN) $(wildcard $(BUSYBOX_BIN))
	@rm -rf $(BUSYBOX_ROOTFS_DIR)
	@mkdir -p $(INITRDS_DIR) $(BUSYBOX_ROOTFS_DIR)/bin $(BUSYBOX_ROOTFS_DIR)/dev $(BUSYBOX_ROOTFS_DIR)/proc $(BUSYBOX_ROOTFS_DIR)/tmp
	@echo "milo-initrd-v1" > $(BUSYBOX_ROOTFS_DIR)/init.txt
	@install -m 0755 $(BUILD_DIR)/user/hello $(BUSYBOX_ROOTFS_DIR)/bin/hello
	@if [ -f "$(BUILD_DIR)/user/hello_musl" ]; then \
		install -m 0755 $(BUILD_DIR)/user/hello_musl $(BUSYBOX_ROOTFS_DIR)/bin/hello_musl; \
	fi
	@if [ -f "$(BUSYBOX_BIN)" ]; then \
		install -m 0755 $(BUSYBOX_BIN) $(BUSYBOX_ROOTFS_DIR)/bin/busybox && \
		ln -sf busybox $(BUSYBOX_ROOTFS_DIR)/bin/sh && \
		for app in ls cat echo pwd ps uname grep wc head tail mkdir rmdir rm cp mv touch free sleep true false; do \
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

$(BASH_CPIO): $(BUILD_DIR)/user/hello $(MUSL_USER_BIN) $(BASH_BIN)
	@rm -rf $(BASH_ROOTFS_DIR)
	@mkdir -p $(INITRDS_DIR) $(BASH_ROOTFS_DIR)/bin $(BASH_ROOTFS_DIR)/dev $(BASH_ROOTFS_DIR)/proc $(BASH_ROOTFS_DIR)/tmp
	@echo "milo-initrd-v1" > $(BASH_ROOTFS_DIR)/init.txt
	@install -m 0755 $(BUILD_DIR)/user/hello $(BASH_ROOTFS_DIR)/bin/hello
	@if [ -f "$(BUILD_DIR)/user/hello_musl" ]; then \
		install -m 0755 $(BUILD_DIR)/user/hello_musl $(BASH_ROOTFS_DIR)/bin/hello_musl; \
	fi
	@install -m 0755 $(BASH_BIN) $(BASH_ROOTFS_DIR)/bin/bash
	@ln -sf bash $(BASH_ROOTFS_DIR)/bin/sh
	@cd $(BASH_ROOTFS_DIR) && find . | cpio -o -H newc > ../../$(BASH_CPIO) 2>/dev/null

setup-bash: $(BASH_CPIO)

-include $(DEPS)
-include $(HOST_TEST_DEPS)
-include $(HOST_COV_DEPS)

clean:
	rm -rf $(BUILD_DIR)/kernel $(BUILD_DIR)/arch \
	       $(BUILD_DIR)/$(TARGET) $(BUILD_DIR)/kernel.elf \
	       $(HOST_BUILD_DIR) $(HOST_COV_DIR) $(REPORTS_DIR) \
	       $(BUSYBOX_ROOTFS_DIR) $(BASH_ROOTFS_DIR) arch/memory_config.hpp

distclean:
	rm -rf $(BUILD_DIR) arch/memory_config.hpp compile_commands.json $(DISK_IMG)

# QEMU Executable & Flags
QEMU      ?= qemu-system-aarch64
QEMUFLAGS := -M virt \
             -cpu cortex-a57 \
			 -m 256M \
			 -nographic \
			 -kernel $(BUILD_DIR)/$(TARGET) \
             -initrd $(INITRD) \
			 -global virtio-mmio.force-legacy=false \
			 -drive file=disk.img,format=raw,if=none,id=hd0 \
			 -device virtio-blk-device,drive=hd0
GDB       ?= gdb-multiarch

DISK_IMG ?= disk.img
DISK_SIZE_MB ?= 4

$(DISK_IMG):
	dd if=/dev/zero of=$@ bs=1M count=$(DISK_SIZE_MB)

.PHONY: disk
disk: $(DISK_IMG)

run: $(BUILD_DIR)/$(TARGET) $(INITRD) $(DISK_IMG)
	$(QEMU) $(QEMUFLAGS)

debug: $(BUILD_DIR)/$(TARGET) $(INITRD)
	@echo "Starting QEMU in debug mode. Connect with: make gdb"
	$(QEMU) $(QEMUFLAGS) -s -S

# Attach GDB using the ELF symbol file
gdb: $(BUILD_DIR)/kernel.elf
	$(GDB) $(BUILD_DIR)/kernel.elf -ex "target remote localhost:1234" -ex "layout split"

# Find all project source/header files for formatting/linting targets
HDRS := $(shell find kernel arch tests -name '*.hpp' -o -name '*.h' 2>/dev/null)
TEST_FORMAT_SRCS := $(shell find tests -name '*.cpp' 2>/dev/null)

# --- Compilation Database Generation (incremental, without clean rebuilds) ---
compile_commands.json: Makefile kernel/kernel.mk arch/$(ARCH)/arch.mk arch/memory_config.hpp scripts/generate_compile_commands.py
	@echo "[COMPILE-DB] Updating compile_commands.json..."
	@python3 scripts/generate_compile_commands.py \
		--cxx "$(CXX)" \
		--cxxflags "$(CXXFLAGS)" \
		--kernel-includes "-Ikernel -Iarch/$(ARCH)" \
		--build-dir "$(BUILD_DIR)" \
		--kernel-srcs $(SRCS_CXX) \
		--host-cxx "$(HOST_CXX)" \
		--host-cxxflags "$(HOST_CXXFLAGS)" \
		--host-includes "$(HOST_INCLUDES)" \
		--host-build-dir "$(HOST_BUILD_DIR)" \
		--host-srcs $(HOST_ONLY_TEST_SRCS) \
		--output $@

# --- Code Quality Targets ---
lint: compile_commands.json
	@echo "[LINT] Running clang-tidy on C++ sources..."
	@python3 scripts/generate_quality_report.py -j $(NPROC)

format:
	@echo "[FORMAT] Formatting sources with clang-format..."
	clang-format -i $(SRCS_CXX) $(TEST_FORMAT_SRCS) $(SRCS_C) $(HDRS)

format-check:
	@echo "[FORMAT-CHECK] Checking source formatting..."
	clang-format --dry-run --Werror $(SRCS_CXX) $(TEST_FORMAT_SRCS) $(SRCS_C) $(HDRS)

lint-report: compile_commands.json
	@python3 scripts/generate_quality_report.py -j $(NPROC)

quality:
	@$(MAKE) $(if $(findstring -j,$(MAKEFLAGS)),,-j$(NPROC)) format-check lint-report test-host

setup-hooks:
	git config core.hooksPath .githooks
	chmod +x .githooks/pre_commit
	@echo "[HOOKS] Configured git core.hooksPath -> .githooks"

commit: quality
	git commit
