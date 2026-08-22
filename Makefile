TARGET        := aarch64-unknown-none
AS            := aarch64-linux-gnu-as
LD            := aarch64-linux-gnu-ld
QEMU          := qemu-system-aarch64

LINKER_SCRIPT := linker.ld
BOOT_SRC      := boot.s
BOOT_OBJ      := boot.o
RUST_LIB      := target/$(TARGET)/release/libkernel_core.a
ELF           := kernel.elf

# Safe file discovery for Rust sources
RUST_SRCS     := $(shell find . -type f -name '*.rs' 2>/dev/null) Cargo.toml

.PHONY: all run run-el2 debug clean

all: $(ELF)

$(BOOT_OBJ): $(BOOT_SRC)
	$(AS) -g $(BOOT_SRC) -o $(BOOT_OBJ)

$(RUST_LIB): $(RUST_SRCS)
	cargo build --package kernel-core --target $(TARGET) --release

$(ELF): $(BOOT_OBJ) $(RUST_LIB) $(LINKER_SCRIPT)
	$(LD) --no-warn-rwx-segments -T $(LINKER_SCRIPT) $(BOOT_OBJ) $(RUST_LIB) -o $@

# Standard run (starts at EL1)
run: $(ELF)
	$(QEMU) -M virt -cpu cortex-a53 -display none -serial stdio -kernel $(ELF)

# Boot at EL2 to test el2_to_el1 lowering
run-el2: $(ELF)
	$(QEMU) -M virt,virtualization=on -cpu cortex-a53 -display none -serial stdio -kernel $(ELF)

debug: $(ELF)
	$(QEMU) -M virt,virtualization=on -cpu cortex-a53 -display none -serial stdio \
		-kernel $(ELF) -gdb tcp::12345 -S \
		-d in_asm,int -D qemu.log

clean:
	cargo clean
	rm -f $(BOOT_OBJ) $(ELF)