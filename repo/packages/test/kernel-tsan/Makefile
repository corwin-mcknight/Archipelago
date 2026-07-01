# Host TSan lane: real-thread concurrency stress over ktl::atomic. Separate package from the host
# runner and the fuzzer because TSan cannot co-instrument with ASan or libFuzzer. Build tool: stays
# on the host, not installed into the sysroot.

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

KSRC   := $(LIVE_SOURCES)
OBJDIR := $(BUILD_DIR)/obj/test/kernel-tsan
TSAN   := $(OBJDIR)/tsan-atomic

INC_KERNEL := -I $(KSRC)/includes/std -I $(KSRC)/includes

# Kernel dialect (no exceptions/rtti, strict aliasing) but NOT -ffreestanding: this is a hosted driver
# whose main()/pthreads/stdio are the test harness, and -ffreestanding makes the compiler mangle main
# so the C runtime can't find it. The only code under test is the ktl::atomic header -- pure __atomic_*
# builtins TSan instruments identically with or without freestanding, so no fidelity is lost. Targets
# only lock-free KTL data structures; the sync primitives stay on the QEMU tier.
SAN   := -fsanitize=thread -fno-sanitize-recover=all
FLAGS := -std=c++20 -g -O1 -fno-exceptions -fno-rtti \
	-fno-omit-frame-pointer -fvisibility=hidden -fvisibility-inlines-hidden -fstrict-aliasing $(SAN)

SRC := $(KSRC)/tests/tsan/atomic_tsan.cpp

pkg_get_source:
	@true

pkg_configure:
	@true

pkg_build:
	@echo "[plume] Building atomic TSan stress harness"
	mkdir -p $(OBJDIR)
	$(CXX) $(FLAGS) $(INC_KERNEL) -c $(SRC) -o $(OBJDIR)/atomic_tsan.o
	$(CXX) $(SAN) -fuse-ld=lld -lpthread $(OBJDIR)/atomic_tsan.o -o $(TSAN)

pkg_install:
	@echo "[plume] Installing atomic TSan stress harness"
	mkdir -p $(TOOL_INSTALL)/kernel-tsan
	cp $(TSAN) $(TOOL_INSTALL)/kernel-tsan/tsan-atomic
