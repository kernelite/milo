TARGET        := aarch64-unknown-none
AS            := aarch64-linux-gnu-as
LD            := aarch64-linux-gnu-ld
QEMU          := qemu-system-aarch64

LINKER_SCRIPT := linker.ld
BOOT_SRC      := boot.s
BOOT_OBJ      := boot.o
RUST_LIB      := target/$(TARGET)/release/libkernel_core.a
ELF           := kernel.elf

.PHONY: all run clean

all: $(ELF)

$(BOOT_OBJ): $(BOOT_SRC)
	$(AS) $(BOOT_SRC) -o $(BOOT_OBJ)

$(RUST_LIB): $(shell find crates -type f) Cargo.toml
	cargo build --package kernel-core --target $(TARGET) --release

$(ELF): $(BOOT_OBJ) $(RUST_LIB) $(LINKER_SCRIPT)
	$(LD) --no-warn-rwx-segments -T $(LINKER_SCRIPT) $(BOOT_OBJ) $(RUST_LIB) -o $@

run: $(ELF)
	$(QEMU) -M virt -cpu cortex-a53 -display none -serial stdio -kernel $(ELF)

clean:
	cargo clean
	rm -f $(BOOT_OBJ) $(ELF)