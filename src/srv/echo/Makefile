CC := clang
LD := ld.lld

TARGET_EXEC ?= echo.elf

BUILD_DIR ?= ./
OBJ_DIR   ?= $(BUILD_DIR)/obj/srv/echo
SRC_DIRS  ?= ./

# Plume exports ARCH, TRIPLE and SYSROOT; the defaults cover standalone invocation.
ARCH ?= x86_64
SYSROOT ?= $(BUILD_DIR)/sysroot

ifeq ($(ARCH),x86_64)
TRIPLE ?= x86_64-unknown-none
ARCH_CFLAGS  := -m64 --target=$(TRIPLE)
LD_EMULATION := elf_x86_64
else ifeq ($(ARCH),riscv64)
TRIPLE ?= riscv64-unknown-none
ARCH_CFLAGS  := --target=$(TRIPLE) -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
LD_EMULATION := elf64lriscv
else
$(error unsupported ARCH '$(ARCH)')
endif

# No board facts are compiled in, so this builds once per architecture. The runtime -- _start,
# syscall stubs, the linker script -- comes from the installed lib/crt package.
SRCS := $(wildcard $(SRC_DIRS)/*.c)
OBJS := $(SRCS:%=$(OBJ_DIR)/%.o)
DEPS := $(filter %.c.d,$(OBJS:.o=.d))

override CFLAGS += \
	-MMD -MP \
	-g -Og \
	-std=c11 \
	-ffreestanding \
	-fno-builtin \
	-nostdlib \
	-fno-stack-protector \
	-fno-omit-frame-pointer \
	-fno-PIE \
	-fno-PIC \
	$(ARCH_CFLAGS) \
	-I $(SYSROOT)/usr/include

override CWARNINGS += -Wall -Wextra -Wconversion -Wshadow -Wpointer-arith -Wunused

LDFLAGS := -nostdlib \
	-static \
	-no-pie \
	-m $(LD_EMULATION) \
	-z max-page-size=0x1000 \
	-z noexecstack \
	-T $(SYSROOT)/usr/lib/user.ld

$(OBJ_DIR)/$(TARGET_EXEC): $(OBJS)
	@echo "Linking $(TARGET_EXEC)..."
	@$(LD) $(OBJS) $(SYSROOT)/usr/lib/libcrt.a $(LDFLAGS) -o $@

$(OBJ_DIR)/%.c.o: %.c
	@echo -n "+"
	@$(MKDIR_P) $(dir $@)
	@$(CC) $(CPPFLAGS) $(CFLAGS) $(CWARNINGS) -c $< -o $@

.PHONY: clean

clean:
	$(RM) -r $(OBJ_DIR)

-include $(DEPS)

MKDIR_P ?= mkdir -p
