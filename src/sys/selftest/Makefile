# The user-mode self-check payload. Everything but the names comes from the shared user-program
# fragment the lib/crt package installs beside user.ld.
TARGET_EXEC ?= selftest.elf

BUILD_DIR ?= ./
OBJ_DIR   ?= $(BUILD_DIR)/obj/sys/selftest
SYSROOT   ?= $(BUILD_DIR)/sysroot

include $(SYSROOT)/usr/lib/user.mk
