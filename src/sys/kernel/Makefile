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
BOARD ?= pc
TRIPLE ?= x86_64-unknown-none
ARCH_DEFINES  := -D ARCH_X86 -D ARCH_X86_64 -D__x86_64__
ARCH_CXXFLAGS := -m64 --target=$(TRIPLE) \
    -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel
LD_EMULATION  := elf_x86_64
else ifeq ($(ARCH),riscv64)
BOARD ?= virt
TRIPLE ?= riscv64-unknown-none
ARCH_DEFINES  := -D ARCH_RISCV64
# No hard float in-kernel (lp64 ABI, no f/d extensions); medany so the higher-half kernel links.
ARCH_CXXFLAGS := --target=$(TRIPLE) -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
LD_EMULATION  := elf64lriscv
else
$(error unsupported ARCH '$(ARCH)')
endif

# Glob all CPP files in src. Boards live under $(ARCH)/platforms/<board>/ and are
# selected one at a time below, so they are excluded from the arch-wide glob --
# otherwise every board on this arch would be compiled into the image.
ARCH_CPP_SRCS := $(shell find $(SRC_DIRS)/$(ARCH)/ -name *.cpp -not -path '*/tests/*' -not -path '*/platforms/*')
# .s is NASM (x86_64 only); .S is GNU syntax assembled by the C compiler's integrated assembler.
ARCH_ASM_SRCS := $(shell find $(SRC_DIRS)/$(ARCH)/ -name *.s -not -path '*/platforms/*') \
	$(shell find $(SRC_DIRS)/$(ARCH)/ -name *.S -not -path '*/platforms/*')
ARCH_SRCS := $(ARCH_CPP_SRCS) $(ARCH_ASM_SRCS)
# Boot protocol: one implementation of kernel::boot::collect() under boot/<protocol>/.
# Everything that knows the protocol's types lives there; core/ sees only boot_info.
BOOT ?= limine
BOOT_SRCS := $(shell find $(SRC_DIRS)/boot/$(BOOT) -name *.cpp -not -path '*/tests/*')
# Board: device addresses, clock rates, and the debug-exit device, under
# $(ARCH)/platforms/<board>/. Plume exports BOARD for the kernel package; the
# per-arch default above covers standalone invocation. A board belongs to one
# arch -- which is why it nests inside one -- so board code may use that
# architecture's instructions directly.
PLATFORM_DIR  := $(SRC_DIRS)/$(ARCH)/platforms/$(BOARD)
PLATFORM_SRCS := $(shell find $(PLATFORM_DIR) -name *.cpp -not -path '*/tests/*') \
	$(shell find $(PLATFORM_DIR) -name *.S -not -path '*/tests/*')
CORE_SRCS := $(shell find $(SRC_DIRS)/core -name *.cpp -not -path '*/tests/*')
TASK_SRCS := $(shell find $(SRC_DIRS)/task -name *.cpp -not -path '*/tests/*')
OBJ_SRCS := $(shell find $(SRC_DIRS)/obj -name *.cpp -not -path '*/tests/*')
CRASH_SRCS := $(shell find $(SRC_DIRS)/crash -name *.cpp -not -path '*/tests/*')
ELF_SRCS := $(shell find $(SRC_DIRS)/elf -name *.cpp -not -path '*/tests/*')
SYSCALL_SRCS := $(shell find $(SRC_DIRS)/syscalls -name *.cpp -not -path '*/tests/*')
MM_SRCS := $(shell find $(SRC_DIRS)/mm -name *.cpp -not -path '*/tests/*')
SHELL_SRCS := $(shell find $(SRC_DIRS)/shell -name *.cpp -not -path '*/tests/*')
# Kernel-side tests are the freestanding ones: those under tests/freestanding/ plus the arch tests
# under $(ARCH)/tests/. Hosted tests (tests/*.cpp) are host-tier only -- run by the host runner
# (kernel-testrunner), never built into the kernel image -- and tests/runner/ is the host harness.
# The directory is the routing: a test changes tiers by moving directory, with no list to keep in sync.
TEST_SRCS := $(shell find $(SRC_DIRS) -type f -path '*/tests/freestanding/*.cpp') \
	$(shell find $(SRC_DIRS) -type f -path '*/$(ARCH)/tests/*.cpp') \
	$(shell find $(PLATFORM_DIR) -type f -path '*/tests/*.cpp')
SRCS := $(CORE_SRCS) $(TASK_SRCS) $(OBJ_SRCS) $(CRASH_SRCS) $(ELF_SRCS) $(SYSCALL_SRCS) $(BOOT_SRCS) $(PLATFORM_SRCS) \
	$(ARCH_SRCS) $(MM_SRCS) $(SHELL_SRCS) $(TEST_SRCS)

OBJS := $(SRCS:%=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

override ASFLAGS += -g -f elf64



override CWARNINGS += -Wall -Weffc++ -pedantic  \
-Wextra -Waggregate-return -Wcast-align \
-Wcast-qual -Wconversion \
-Wcomma \
-Wdeprecated-copy-with-dtor \
-Wdisabled-optimization \
-Wexit-time-destructors \
-Wfloat-equal -Wformat=2 \
-Wformat-nonliteral -Wformat-security  \
-Wformat-y2k \
-Wimplicit  -Wimport  -Winit-self  -Winline \
-Winvalid-pch   \
-Wglobal-constructors \
-Wlong-long \
-Wmissing-field-initializers -Wmissing-format-attribute   \
-Wmissing-include-dirs -Wmissing-noreturn -Wmissing-prototypes \
-Wmissing-variable-declarations \
-Wnewline-eof -Wnrvo \
-Wpointer-arith \
-Wredundant-decls \
-Wshadow -Wshadow-uncaptured-local -Wstack-protector \
-Wstrict-aliasing=2 -Wswitch-default \
-Wswitch-enum \
-Wunreachable-code -Wunreachable-code-break -Wunreachable-code-return -Wunused \
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
