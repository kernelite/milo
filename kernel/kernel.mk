SRCS_CXX += kernel/main.cpp \
			kernel/runtime/cxx_abi.cpp \
			kernel/core/syscall_dispatcher.cpp \
			arch/$(ARCH)/cpu.cpp \
			kernel/shell/shell.cpp \
			kernel/mm/pfa.cpp