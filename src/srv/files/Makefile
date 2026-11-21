TARGET_EXEC := files.elf
BUILD_DIR ?= ./
OBJ_DIR ?= $(BUILD_DIR)/obj/srv/files
SYSROOT ?= $(BUILD_DIR)/sysroot
USER_LIBS := $(SYSROOT)/usr/lib/libinitrd.a
CPPFLAGS += -I include
include $(SYSROOT)/usr/lib/user.mk
