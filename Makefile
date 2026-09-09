ARCH ?= aarch64-linux-gnu
TARGET := kernel.bin

# Toolchain targeting bare-metal
PREFIX  := $(ARCH)-
CXX     := $(PREFIX)g++
AS      := $(PREFIX)as
LD      := $(PREFIX)ld

BUILD_DIR := build

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

.PHONY: all clean

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Ikernel -Iarch/$(ARCH) -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR)

# QEMU Executable & Flags
QEMU      ?= qemu-system-aarch64
QEMUFLAGS := -M virt -cpu cortex-a53 -nographic -kernel $(BUILD_DIR)/$(TARGET)
GDB       ?= gdb-multiarch

.PHONY: run debug gdb

# Run kernel directly inside QEMU
run: $(BUILD_DIR)/$(TARGET)
	$(QEMU) $(QEMUFLAGS)

# Start QEMU halted on startup (-S) listening for GDB on port 1234 (-s)
debug: $(BUILD_DIR)/$(TARGET)
	@echo "Starting QEMU in debug mode. Connect with: make gdb"
	$(QEMU) $(QEMUFLAGS) -s -S

# Launch GDB pre-configured and attached to the running QEMU instance
gdb: $(BUILD_DIR)/$(TARGET)
	$(GDB) $(BUILD_DIR)/$(TARGET) -ex "target remote localhost:1234" -ex "layout split"

# Find all header files for formatting/linting targets
HDRS := $(shell find kernel arch -name '*.hpp' -o -name '*.h')

# --- Compilation Database Generation ---
.PHONY: compile_commands.json
compile_commands.json: clean
	@echo "[BEAR] Generating compilation database..."
	bear -- $(MAKE) all

# --- Code Quality Targets ---
.PHONY: lint format format-check

# Run clang-tidy against all C++ source files using compile_commands.json
lint: compile_commands.json
	@echo "[LINT] Running clang-tidy on C++ sources..."
	clang-tidy -p . $(SRCS_CXX)

# Automatically format all source and header files in-place
format:
	@echo "[FORMAT] Formatting sources with clang-format..."
	clang-format -i $(SRCS_CXX) $(HDRS)

# CI check: Fail if any file does not adhere to .clang-format
format-check:
	@echo "[FORMAT-CHECK] Checking source formatting..."
	clang-format --dry-run --Werror $(SRCS_CXX) $(HDRS)