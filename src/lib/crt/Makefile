CC := clang
AR := llvm-ar

TARGET_LIB ?= libcrt.a

BUILD_DIR ?= ./
OBJ_DIR   ?= $(BUILD_DIR)/obj/lib/crt
SRC_DIRS  ?= ./

# Plume exports ARCH, TRIPLE and SYSROOT; the defaults cover standalone invocation.
ARCH ?= x86_64
SYSROOT ?= $(BUILD_DIR)/sysroot

# TRIPLE, ARCH_FLAGS, and LD_EMULATION come from the shared per-arch contract.
include $(dir $(lastword $(MAKEFILE_LIST)))arch.mk

# No board facts are compiled in, so this builds once per architecture.
SRCS := $(wildcard $(SRC_DIRS)/*.c) $(wildcard $(SRC_DIRS)/$(ARCH)/*.S)
OBJS := $(SRCS:%=$(OBJ_DIR)/%.o)
DEPS := $(filter %.c.d,$(OBJS:.o=.d))

# The syscall numbers come from the installed public ABI header, not from the kernel source tree:
# this runtime is built against the same contract any other user code would be.
override CFLAGS += \
	-MMD -MP \
	-g -Og \
	-std=c11 \
	-ffreestanding \
	-fno-builtin \
	-nostdlib \
	-fno-stack-protector \
	-fno-omit-frame-pointer \
	-fno-PIE \
	-fno-PIC \
	$(ARCH_FLAGS) \
	-I include \
	-I $(SYSROOT)/usr/include

override CWARNINGS += -Wall -Wextra -Wconversion -Wshadow -Wpointer-arith -Wunused

$(OBJ_DIR)/$(TARGET_LIB): $(OBJS)
	@echo "Archiving $(TARGET_LIB)..."
	@$(AR) rcs $@ $(OBJS)

$(OBJ_DIR)/%.S.o: %.S
	@echo -n "A"
	@$(MKDIR_P) $(dir $@)
	@$(CC) $(ARCH_FLAGS) -g -c $< -o $@

$(OBJ_DIR)/%.c.o: %.c
	@echo -n "+"
	@$(MKDIR_P) $(dir $@)
	@$(CC) $(CPPFLAGS) $(CFLAGS) $(CWARNINGS) -c $< -o $@

.PHONY: clean

clean:
	$(RM) -r $(OBJ_DIR)

-include $(DEPS)

MKDIR_P ?= mkdir -p
