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

# TRIPLE, ARCH_FLAGS, and LD_EMULATION come from the shared per-arch contract, installed (and
# maintained) beside this file.
include $(dir $(lastword $(MAKEFILE_LIST)))arch.mk

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
