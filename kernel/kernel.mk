SRCS_CXX += kernel/main.cpp \
			kernel/runtime/cxx_abi.cpp \
            kernel/core/syscall_dispatcher.cpp \
            arch/$(ARCH)/cpu_aarch64.cpp \
            kernel/shell/shell.cpp