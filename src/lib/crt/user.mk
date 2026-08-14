# The shared user-program build fragment, installed beside user.ld. A program's Makefile sets
# TARGET_EXEC and OBJ_DIR and includes this from the sysroot; the sources are whatever .c/.cpp
# files sit beside it. The runtime -- _start, syscall stubs, the linker script -- comes from the
# installed lib/crt package, and the syscall numbers from the installed public ABI headers, not
# from the kernel source tree: every user program is built against the same contract.
#
# No board facts are compiled in, so unlike the kernel a user program builds once per architecture.
CC  := clang
CXX := clang++
LD  := ld.lld

BUILD_DIR ?= ./
SRC_DIRS  ?= ./

# Plume exports ARCH, TRIPLE and SYSROOT; the defaults cover standalone invocation.
ARCH ?= x86_64
SYSROOT ?= $(BUILD_DIR)/sysroot

ifeq ($(ARCH),x86_64)
TRIPLE ?= x86_64-unknown-none
# Standard x86_64 ABI, vector instructions included: the kernel enables SSE for user mode and
# carries FP/SIMD state per thread across context switches.
ARCH_FLAGS   := -m64 --target=$(TRIPLE)
LD_EMULATION := elf_x86_64
else ifeq ($(ARCH),riscv64)
TRIPLE ?= riscv64-unknown-none
# Integer registers only: the kernel does not save F/D state, and the rv64imac -march keeps the
# compiler from emitting it.
ARCH_FLAGS   := --target=$(TRIPLE) -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
LD_EMULATION := elf64lriscv
else
$(error unsupported ARCH '$(ARCH)')
endif

SRCS := $(wildcard $(SRC_DIRS)/*.c) $(wildcard $(SRC_DIRS)/*.cpp)
OBJS := $(SRCS:%=$(OBJ_DIR)/%.o)
DEPS := $(filter %.d,$(OBJS:.o=.d))

COMMON_FLAGS := \
	-MMD -MP \
	-g -Og \
	-ffreestanding \
	-fno-builtin \
	-nostdlib \
	-fno-stack-protector \
	-fno-omit-frame-pointer \
	-fno-PIE \
	-fno-PIC \
	$(ARCH_FLAGS) \
	-I $(SYSROOT)/usr/include

override CFLAGS   += -std=c11 $(COMMON_FLAGS)
override CXXFLAGS += -std=c++20 -fno-exceptions -fno-rtti $(COMMON_FLAGS)

override CWARNINGS += -Wall -Wextra -Wconversion -Wshadow -Wpointer-arith -Wunused

LDFLAGS := -nostdlib \
	-static \
	-no-pie \
	-m $(LD_EMULATION) \
	-z max-page-size=0x1000 \
	-z noexecstack \
	-T $(SYSROOT)/usr/lib/user.ld

$(OBJ_DIR)/$(TARGET_EXEC): $(OBJS)
	@echo "Linking $(TARGET_EXEC)..."
	@$(LD) $(OBJS) $(SYSROOT)/usr/lib/libcrt.a $(LDFLAGS) -o $@

$(OBJ_DIR)/%.c.o: %.c
	@echo -n "+"
	@$(MKDIR_P) $(dir $@)
	@$(CC) $(CPPFLAGS) $(CFLAGS) $(CWARNINGS) -c $< -o $@

$(OBJ_DIR)/%.cpp.o: %.cpp
	@echo -n "+"
	@$(MKDIR_P) $(dir $@)
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CWARNINGS) -c $< -o $@

.PHONY: clean

clean:
	$(RM) -r $(OBJ_DIR)

-include $(DEPS)

MKDIR_P ?= mkdir -p
