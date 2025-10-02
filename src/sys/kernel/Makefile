AS  := nasm
CC  := clang
CXX := clang++
LD  := ld.lld

TARGET_EXEC ?= kernel.elf

BUILD_DIR ?= ./
OBJ_DIR   ?= $(BUILD_DIR)/obj/sys/kernel
SRC_DIRS  ?= ./

ARCH := x86_64

# Glob all CPP files in src
ARCH_CPP_SRCS := $(shell find $(SRC_DIRS)/$(ARCH)/ -name *.cpp -not -path '*/tests/*')
ARCH_ASM_SRCS := $(shell find $(SRC_DIRS)/$(ARCH)/ -name *.s)
ARCH_SRCS := $(ARCH_CPP_SRCS) $(ARCH_ASM_SRCS)
CORE_SRCS := $(shell find $(SRC_DIRS)/core -name *.cpp -not -path '*/tests/*')
STD_SRCS := $(shell find $(SRC_DIRS)/std -name *.cpp -not -path '*/tests/*')
TEST_SRCS := $(shell find $(SRC_DIRS) -type f -path '*/tests/*.cpp')
SRCS := $(CORE_SRCS) $(ARCH_SRCS) $(STD_SRCS) $(TEST_SRCS)

OBJS := $(SRCS:%=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

override ASFLAGS += -g -f elf64

override CFLAGS += -g -DLIMINE_API_REVISION=3  -D__x86_64__


override CWARNINGS += -Wall -Weffc++ -pedantic  \
-Wextra -Waggregate-return -Wcast-align \
-Wcast-qual -Wconversion \
-Wdisabled-optimization \
-Wfloat-equal -Wformat=2 \
-Wformat-nonliteral -Wformat-security  \
-Wformat-y2k \
-Wimplicit  -Wimport  -Winit-self  -Winline \
-Winvalid-pch   \
-Wlong-long \
-Wmissing-field-initializers -Wmissing-format-attribute   \
-Wmissing-include-dirs -Wmissing-noreturn \
-Wpointer-arith \
-Wredundant-decls \
-Wshadow -Wstack-protector \
-Wstrict-aliasing=2 -Wswitch-default \
-Wswitch-enum \
-Wunreachable-code -Wunused \
-Wunused-parameter \
-Wvariadic-macros \
-Wwrite-strings \
# -Wpacked  -Wpadded 

override CXXFLAGS += \
	-MMD -MP \
	-g \
	-Og \
	-fvisibility=hidden \
	-pipe \
    -std=c++20 \
    -ffreestanding \
	-fno-builtin \
	-nostdlib \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
	-fno-rtti \
    -fno-PIE \
    -fno-PIC \
    -m64 \
    --target=x86_64-unknown-none \
    -mno-80387 \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -mno-red-zone \
    -mcmodel=kernel \
	-D OPTION_KTL_HARDEN \
	-D ARCH_X86 \
	-D ARCH_X86_64 \
	-DLIMINE_API_REVISION=3 \
	-D__x86_64__ \
	-I $(PWD)/includes/std \
	-I $(PWD)/includes

LDFLAGS := -nostdlib \
    -static \
    -m elf_x86_64 \
    -z max-page-size=0x1000 \
	-no-pie \
    -T $(ARCH)/linker.ld

$(OBJ_DIR)/$(TARGET_EXEC): $(OBJS)
	@echo ""
	@echo "Linking $(TARGET_EXEC)..."
	@$(LD) $(OBJS) $(LDFLAGS) -o $@ 

# assembly
$(OBJ_DIR)/%.s.o: %.s
	@echo -n "A"
	@$(MKDIR_P) $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@

# c source
$(OBJ_DIR)/%.c.o: %.c
	@echo -n "C"
	@$(MKDIR_P) $(dir $@)
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# c++ source
$(OBJ_DIR)/%.cpp.o: %.cpp
	@echo -n "+"
	@$(MKDIR_P) $(dir $@)
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CWARNINGS) -c $< -o $@


.PHONY: clean

clean:
	$(RM) -r $(OBJ_DIR)
	
install:
	$(MKDIR_P) $(BUILD_DIR)/sysroot/boot
	cp $(OBJ_DIR)/$(TARGET_EXEC) $(BUILD_DIR)/sysroot/boot/kernel.elf

-include $(DEPS)

MKDIR_P ?= mkdir -p
