SRCS_CXX += kernel/main.cpp \
            kernel/runtime/cxx_abi.cpp \
            kernel/core/syscall_dispatcher.cpp \
            arch/$(ARCH)/cpu.cpp \
            kernel/shell/shell.cpp \
            kernel/mm/pfa.cpp \
            kernel/mm/address_space.cpp \
            kernel/boot/fdt.cpp \
            kernel/fs/cpio.cpp \
            kernel/loader/elf64.cpp \
            kernel/loader/elf_loader.cpp \
            kernel/process/stack_builder.cpp \
            kernel/process/process.cpp