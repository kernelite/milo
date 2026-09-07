# Populate assembly sources specific to this target architecture
SRCS_ASM += arch/$(ARCH)/boot.s
SRCS_CXX += arch/$(ARCH)/uart_console.cpp
