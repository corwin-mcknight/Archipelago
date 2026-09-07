# The coordinator. Everything but the names comes from the shared user-program fragment the
# lib/crt package installs beside user.ld.
TARGET_EXEC ?= init.link
.DEFAULT_GOAL := boot-image

BUILD_DIR ?= ./
OBJ_DIR   ?= $(BUILD_DIR)/obj/sys/init
SYSROOT   ?= $(BUILD_DIR)/sysroot

USER_LIBS := $(SYSROOT)/usr/lib/libelf.a
include $(SYSROOT)/usr/lib/user.mk

# The link intermediate is retained for debugging but never installed as a boot module.
ifeq ($(ARCH),riscv64)
INIT_MACHINE := 2
else
INIT_MACHINE := 1
endif
LDFLAGS := $(filter-out -T $(SYSROOT)/usr/lib/user.ld,$(LDFLAGS)) -T init.ld --defsym=INIT_MACHINE=$(INIT_MACHINE)

.PHONY: boot-image
boot-image: $(OBJ_DIR)/init.bin

$(OBJ_DIR)/$(TARGET_EXEC): init.ld

$(OBJ_DIR)/init.bin: $(OBJ_DIR)/$(TARGET_EXEC)
	llvm-objcopy -O binary $< $@
