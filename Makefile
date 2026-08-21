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

debug:
	aarch64-linux-gnu-as boot.s -o boot.o
	cargo build --package kernel-core --target aarch64-unknown-none --release
	aarch64-linux-gnu-ld --no-warn-rwx-segments -T linker.ld boot.o target/aarch64-unknown-none/release/libkernel_core.a -o kernel.elf
	qemu-system-aarch64 -M virt -cpu cortex-a53 -display none -serial stdio -kernel kernel.elf -gdb tcp::12345 -S

clean:
	cargo clean
	rm -f $(BOOT_OBJ) $(ELF)