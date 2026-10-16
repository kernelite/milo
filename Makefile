ARCH ?= aarch64-linux-gnu
TARGET := kernel.bin

# Toolchain targeting bare-metal
PREFIX  := $(ARCH)-
CXX     := $(PREFIX)g++
AS      := $(PREFIX)as
LD      := $(PREFIX)ld
OBJCOPY := $(PREFIX)objcopy

BUILD_DIR := build
ROOTFS_DIR := $(BUILD_DIR)/rootfs
ROOTFS_CPIO := $(BUILD_DIR)/rootfs.cpio

# Freestanding C++ flags
CXXFLAGS := -std=c++20 -ffreestanding -O2 -Wall -Wextra -fno-use-cxa-atexit \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -MMD -MP -g
ASFLAGS  := -g
LDFLAGS  := -nostdlib -T arch/$(ARCH)/linker.ld

# Source lists appended by arch.mk and kernel.mk
SRCS_CXX :=
SRCS_ASM :=

# Include architecture and core modules
include arch/$(ARCH)/arch.mk
include kernel/kernel.mk

# Map sources to build output objects
OBJS := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(SRCS_CXX)) \
        $(patsubst %.s, $(BUILD_DIR)/%.o, $(SRCS_ASM))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean run debug gdb lint format format-check lint-report quality compile_commands.json ai-dump

all: $(BUILD_DIR)/$(TARGET) $(ROOTFS_CPIO)

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

# CPIO Archive Generation (Minimal RAMDisk Payload)
$(ROOTFS_CPIO):
	@mkdir -p $(ROOTFS_DIR)
	@echo "milo-initrd-v1" > $(ROOTFS_DIR)/init.txt
	@cd $(ROOTFS_DIR) && find . | cpio -o -H newc > ../rootfs.cpio 2>/dev/null

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR) arch/memory_config.hpp

# QEMU Executable & Flags
QEMU      ?= qemu-system-aarch64
QEMUFLAGS := -M virt -cpu cortex-a53 -m 256M -nographic -kernel $(BUILD_DIR)/$(TARGET) -initrd $(ROOTFS_CPIO)
GDB       ?= gdb-multiarch

run: $(BUILD_DIR)/$(TARGET) $(ROOTFS_CPIO)
	$(QEMU) $(QEMUFLAGS)

debug: $(BUILD_DIR)/$(TARGET) $(ROOTFS_CPIO)
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
	clang-format -i $(SRCS_CXX) $(HDRS)

format-check:
	@echo "[FORMAT-CHECK] Checking source formatting..."
	clang-format --dry-run --Werror $(SRCS_CXX) $(HDRS)

lint-report: compile_commands.json
	@python3 scripts/generate_quality_report.py

quality: format-check lint-report

ai-dump:
	./concat_files.sh dump-for-ai.txt arch/ build/ kernel/ Makefile compile_commands.json