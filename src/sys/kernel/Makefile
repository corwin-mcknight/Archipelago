AS  := nasm
CC  := clang
CXX := clang++
LD  := ld.lld

TARGET_EXEC ?= kernel.elf

BUILD_DIR ?= ./
OBJ_DIR   ?= $(BUILD_DIR)/obj/sys/kernel
SRC_DIRS  ?= ./

# Plume exports ARCH and TRIPLE from the target config; defaults cover
# standalone invocation.
ARCH ?= x86_64

ifeq ($(ARCH),x86_64)
TRIPLE ?= x86_64-unknown-none
ARCH_DEFINES  := -D ARCH_X86 -D ARCH_X86_64 -D__x86_64__
ARCH_CXXFLAGS := -m64 --target=$(TRIPLE) \
    -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel
LD_EMULATION  := elf_x86_64
else ifeq ($(ARCH),riscv64)
TRIPLE ?= riscv64-unknown-none
ARCH_DEFINES  := -D ARCH_RISCV64
# No hard float in-kernel (lp64 ABI, no f/d extensions); medany so the higher-half kernel links.
ARCH_CXXFLAGS := --target=$(TRIPLE) -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
LD_EMULATION  := elf64lriscv
else
$(error unsupported ARCH '$(ARCH)')
endif

# Glob all CPP files in src
ARCH_CPP_SRCS := $(shell find $(SRC_DIRS)/$(ARCH)/ -name *.cpp -not -path '*/tests/*')
# .s is NASM (x86_64 only); .S is GNU syntax assembled by the C compiler's integrated assembler.
ARCH_ASM_SRCS := $(shell find $(SRC_DIRS)/$(ARCH)/ -name *.s) \
	$(shell find $(SRC_DIRS)/$(ARCH)/ -name *.S)
ARCH_SRCS := $(ARCH_CPP_SRCS) $(ARCH_ASM_SRCS)
CORE_SRCS := $(shell find $(SRC_DIRS)/core -name *.cpp -not -path '*/tests/*')
STD_SRCS := $(shell find $(SRC_DIRS)/std -name *.cpp -not -path '*/tests/*')
MM_SRCS := $(shell find $(SRC_DIRS)/mm -name *.cpp -not -path '*/tests/*')
SHELL_SRCS := $(shell find $(SRC_DIRS)/shell -name *.cpp -not -path '*/tests/*')
# Kernel-side tests are the freestanding ones: those under tests/freestanding/ plus the arch tests
# under $(ARCH)/tests/. Hosted tests (tests/*.cpp) are host-tier only -- run by the host runner
# (kernel-testrunner), never built into the kernel image -- and tests/runner/ is the host harness.
# The directory is the routing: a test changes tiers by moving directory, with no list to keep in sync.
TEST_SRCS := $(shell find $(SRC_DIRS) -type f -path '*/tests/freestanding/*.cpp') \
	$(shell find $(SRC_DIRS) -type f -path '*/$(ARCH)/tests/*.cpp')
SRCS := $(CORE_SRCS) $(ARCH_SRCS) $(STD_SRCS) $(MM_SRCS) $(SHELL_SRCS) $(TEST_SRCS)

OBJS := $(SRCS:%=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

override ASFLAGS += -g -f elf64

override CFLAGS += -g -DLIMINE_API_REVISION=3 $(ARCH_DEFINES)


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
-Wframe-larger-than=2048

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
	-fno-omit-frame-pointer \
	-fvisibility=hidden -fvisibility-inlines-hidden \
	-fstrict-vtable-pointers \
	-ftrivial-auto-var-init=pattern \
	-fstrict-aliasing -Wstrict-aliasing=2 \
    -fno-stack-protector \
    -fno-stack-check \
	-fno-exceptions \
    -fno-lto \
	-fno-rtti \
    -fno-PIE \
    -fno-PIC \
    $(ARCH_CXXFLAGS) \
    $(ARCH_DEFINES) \
	-D PRODUCT_DEBUG \
	-DLIMINE_API_REVISION=3 \
	-I $(CURDIR)/includes/std \
	-I $(CURDIR)/includes

LDFLAGS := -nostdlib \
    -static \
    -m $(LD_EMULATION) \
    -z max-page-size=0x1000 \
	-no-pie \
    -T $(ARCH)/linker.ld

$(OBJ_DIR)/$(TARGET_EXEC): $(OBJS)
	@echo ""
	@echo "Linking $(TARGET_EXEC)..."
	@$(LD) $(OBJS) $(LDFLAGS) -o $@ 

# assembly (NASM syntax, x86_64 only)
$(OBJ_DIR)/%.s.o: %.s
	@echo -n "A"
	@$(MKDIR_P) $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@

# assembly (GNU syntax, via the C compiler's integrated assembler)
$(OBJ_DIR)/%.S.o: %.S
	@echo -n "A"
	@$(MKDIR_P) $(dir $@)
	@$(CC) $(ARCH_CXXFLAGS) $(ARCH_DEFINES) -g -c $< -o $@

# c source
$(OBJ_DIR)/%.c.o: %.c
	@echo -n "C"
	@$(MKDIR_P) $(dir $@)
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# c++ source
$(OBJ_DIR)/%.cpp.o: %.cpp
	@echo -n "+"
	@$(MKDIR_P) $(dir $@)
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CWARNINGS) -MJ $(OBJ_DIR)/$*.json -c $< -o $@


.PHONY: clean

clean:
	$(RM) -r $(OBJ_DIR)
	
install:
	$(MKDIR_P) $(BUILD_DIR)/sysroot/boot
	cp $(OBJ_DIR)/$(TARGET_EXEC) $(BUILD_DIR)/sysroot/boot/kernel.elf

-include $(DEPS)

MKDIR_P ?= mkdir -p
