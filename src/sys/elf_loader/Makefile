TARGET_EXEC := elf_loader.elf
BUILD_DIR ?= ./
OBJ_DIR ?= $(BUILD_DIR)/obj/sys/elf_loader
SYSROOT ?= $(BUILD_DIR)/sysroot
ARCH ?= x86_64
USER_LIBS := $(SYSROOT)/usr/lib/libelf.a
EXTRA_OBJS := $(OBJ_DIR)/entry.o
include $(SYSROOT)/usr/lib/user.mk

$(OBJ_DIR)/entry.o: $(ARCH)/entry.S
	mkdir -p $(OBJ_DIR)
	$(CC) $(ARCH_FLAGS) -c $< -o $@
