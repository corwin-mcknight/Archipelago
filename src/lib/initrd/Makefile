ifneq ($(filter default undefined,$(origin CXX)),)
CXX := clang++
endif
ifneq ($(filter default undefined,$(origin AR)),)
AR := llvm-ar
endif
ARCH ?= x86_64
BUILD_DIR ?= ./
OBJ_DIR ?= $(BUILD_DIR)/obj/lib/initrd
SYSROOT ?= $(BUILD_DIR)/sysroot
include $(SYSROOT)/usr/lib/arch.mk

override CXXFLAGS += -std=c++20 -g -Og -ffreestanding -fno-builtin -nostdlib -nostdinc++ -fno-stack-protector \
    -fno-exceptions -fno-rtti -fno-PIE -fno-PIC $(ARCH_FLAGS) -I include -MMD -MP
OBJS := $(OBJ_DIR)/reader.o

$(OBJ_DIR)/libinitrd.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

$(OBJ_DIR)/%.o: %.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -Wall -Wextra -c $< -o $@

-include $(OBJS:.o=.d)
