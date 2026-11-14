# Populate assembly sources specific to this target architecture
SRCS_ASM += arch/$(ARCH)/boot.s \
            arch/$(ARCH)/user_test.s
SRCS_CXX += arch/$(ARCH)/uart_console.cpp \
            arch/$(ARCH)/mmu_aarch64.cpp \
            arch/$(ARCH)/trap_aarch64.cpp \
            arch/$(ARCH)/arch_test.cpp \
            kernel/hal/targets/qemu_virt/gicv2.cpp \
            kernel/hal/targets/qemu_virt/timer.cpp