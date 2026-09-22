CXX := clang++
AR := llvm-ar
ARCH ?= x86_64
BUILD_DIR ?= ./
OBJ_DIR ?= $(BUILD_DIR)/obj/lib/initrd
SYSROOT ?= $(BUILD_DIR)/sysroot
include $(SYSROOT)/usr/lib/arch.mk

CXXFLAGS := -std=c++20 -g -Og -ffreestanding -fno-builtin -nostdlib -fno-stack-protector \
    -fno-exceptions -fno-rtti -fno-PIE -fno-PIC $(ARCH_FLAGS) -I include -MMD -MP
OBJS := $(OBJ_DIR)/reader.o

$(OBJ_DIR)/libinitrd.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

$(OBJ_DIR)/%.o: %.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -Wall -Wextra -c $< -o $@

-include $(OBJS:.o=.d)
