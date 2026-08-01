CC  := clang
CXX := clang++
LD  := ld.lld

TARGET_EXEC ?= init.elf

BUILD_DIR ?= ./
OBJ_DIR   ?= $(BUILD_DIR)/obj/sys/init
SRC_DIRS  ?= ./

# Plume exports ARCH, TRIPLE and SYSROOT; the defaults cover standalone invocation.
ARCH ?= x86_64
SYSROOT ?= $(BUILD_DIR)/sysroot

ifeq ($(ARCH),x86_64)
TRIPLE ?= x86_64-unknown-none
ARCH_CXXFLAGS := -m64 --target=$(TRIPLE)
LD_EMULATION  := elf_x86_64
else ifeq ($(ARCH),riscv64)
TRIPLE ?= riscv64-unknown-none
ARCH_CXXFLAGS := --target=$(TRIPLE) -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
LD_EMULATION  := elf64lriscv
else
$(error unsupported ARCH '$(ARCH)')
endif

# Unlike the kernel, init has no board facts compiled in -- it builds once per architecture.
SRCS := $(wildcard $(SRC_DIRS)/*.cpp) $(wildcard $(SRC_DIRS)/$(ARCH)/*.S)
OBJS := $(SRCS:%=$(OBJ_DIR)/%.o)
DEPS := $(filter %.cpp.d,$(OBJS:.o=.d))

# The syscall numbers come from the installed public ABI header, not from the kernel source tree:
# this program is built against the same contract any other user program would be.
override CXXFLAGS += \
	-MMD -MP \
	-g -Og \
	-std=c++20 \
	-ffreestanding \
	-fno-builtin \
	-nostdlib \
	-fno-exceptions \
	-fno-rtti \
	-fno-stack-protector \
	-fno-omit-frame-pointer \
	-fno-PIE \
	-fno-PIC \
	$(ARCH_CXXFLAGS) \
	-I $(SYSROOT)/usr/include

override CWARNINGS += -Wall -Wextra -Wconversion -Wshadow -Wpointer-arith -Wunused

LDFLAGS := -nostdlib \
	-static \
	-no-pie \
	-m $(LD_EMULATION) \
	-z max-page-size=0x1000 \
	-z noexecstack \
	-T linker.ld

$(OBJ_DIR)/$(TARGET_EXEC): $(OBJS)
	@echo "Linking $(TARGET_EXEC)..."
	@$(LD) $(OBJS) $(LDFLAGS) -o $@

$(OBJ_DIR)/%.S.o: %.S
	@echo -n "A"
	@$(MKDIR_P) $(dir $@)
	@$(CC) $(ARCH_CXXFLAGS) -g -c $< -o $@

$(OBJ_DIR)/%.cpp.o: %.cpp
	@echo -n "+"
	@$(MKDIR_P) $(dir $@)
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CWARNINGS) -c $< -o $@

.PHONY: clean

clean:
	$(RM) -r $(OBJ_DIR)

-include $(DEPS)

MKDIR_P ?= mkdir -p
