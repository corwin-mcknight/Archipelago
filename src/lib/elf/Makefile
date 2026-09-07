CXX := clang++
AR := llvm-ar
ARCH ?= x86_64
BUILD_DIR ?= ./
OBJ_DIR ?= $(BUILD_DIR)/obj/lib/elf
SYSROOT ?= $(BUILD_DIR)/sysroot
include $(SYSROOT)/usr/lib/arch.mk

CXXFLAGS := -std=c++20 -g -Og -ffreestanding -fno-builtin -nostdlib -fno-stack-protector \
    -fno-exceptions -fno-rtti -fno-PIE -fno-PIC $(ARCH_FLAGS) -I include -I $(SYSROOT)/usr/include -MMD -MP
OBJS := $(OBJ_DIR)/parse.o $(OBJ_DIR)/spawn.o

$(OBJ_DIR)/libelf.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

$(OBJ_DIR)/%.o: %.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -Wall -Wextra -c $< -o $@

-include $(OBJS:.o=.d)
