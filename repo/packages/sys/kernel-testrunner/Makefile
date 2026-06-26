# Host-tier test runner package.
#
# Builds a native binary that runs kernel logic tests on the host under ASan/UBSan with fork-per-test
# isolation. This is a build tool: it stays on the host and is not installed into the sysroot.
#
# Two compile groups:
#   group A (code under test + the kernel logic it links against) -- the "amplifier": kernel
#     language/semantics flags mirrored, bare-metal ABI/codegen flags dropped, sanitizers on, NO
#     -ftrivial-auto-var-init so uninitialised reads are caught. Compiled with the kernel include path.
#   group B (harness) -- ordinary host C++; uses libc (fork/wait/longjmp/stdio). Compiled WITHOUT the
#     kernel <std> include path so its <cstdio>/<cstdlib> resolve to the system headers.

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

KSRC    := $(LIVE_SOURCES)
OBJDIR  := $(BUILD_DIR)/obj/sys/kernel-testrunner
RUNNER  := $(OBJDIR)/host-test-runner

INC_KERNEL := -I $(KSRC)/includes/std -I $(KSRC)/includes
INC_HOST   := -I $(KSRC)/includes

SAN := -fsanitize=address,undefined -fno-sanitize-recover=all

# The host tier is logic-authoritative, NOT the target ABI: we deliberately do not pretend to be
# x86_64 here (this host may be aarch64). Both are LP64 little-endian, so the integer type model the
# logic depends on is identical; ABI/codegen fidelity is the QEMU tier's job.
AMP_FLAGS := -std=c++20 -g -O1 -ffreestanding -fno-builtin -fno-exceptions -fno-rtti \
	-fno-omit-frame-pointer -fvisibility=hidden -fvisibility-inlines-hidden \
	-fstrict-aliasing -Wstrict-aliasing=2 -Werror=unused-result -Werror=unused-value $(SAN)

HARNESS_FLAGS := -std=c++20 -g -O1 -fno-omit-frame-pointer $(SAN)

RUNNER_DIR := $(KSRC)/tests/runner
HOSTED_DIR := $(KSRC)/tests
FRAGMENT   := $(RUNNER_DIR)/ktests_brackets.ld

# Every .cpp directly under tests/ is host-tier: the directory is the routing, so there is no list to
# keep in sync with the kernel build. Tests that need the kernel environment live in tests/freestanding/
# (compiled only by the kernel); the harness lives in tests/runner/ (compiled with host flags below).
HOSTED_SRCS := $(wildcard $(HOSTED_DIR)/*.cpp)

# Kernel logic the hosted tests link against, compiled with the amplifier. Pure data structures and
# freestanding helpers only -- anything hardware-coupled must be a freestanding (QEMU-tier) test instead.
# std/stdlib.cpp also supplies itoa for the formatter the assertion machinery uses.
SUPPORT_SRCS := $(KSRC)/std/stdlib.cpp $(KSRC)/core/object.cpp $(KSRC)/core/handle_table.cpp \
	$(KSRC)/core/type_registry.cpp

AMP_SRCS := $(HOSTED_SRCS) $(SUPPORT_SRCS)
AMP_OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(basename $(notdir $(AMP_SRCS)))))

pkg_get_source:
	@true

pkg_configure:
	@true

pkg_build:
	@echo "[plume] Building host test runner"
	mkdir -p $(OBJDIR)
	@for src in $(AMP_SRCS); do \
		obj=$(OBJDIR)/$$(basename $$src .cpp).o; \
		echo "  CXX (amplifier) $$src"; \
		$(CXX) $(AMP_FLAGS) $(INC_KERNEL) -c $$src -o $$obj || exit 1; \
	done
	$(CXX) $(HARNESS_FLAGS) $(INC_HOST) -c $(RUNNER_DIR)/host_runner.cpp -o $(OBJDIR)/host_runner.o
	$(CXX) $(SAN) -fuse-ld=lld $(AMP_OBJS) $(OBJDIR)/host_runner.o -Wl,-T,$(FRAGMENT) -o $(RUNNER)

pkg_install:
	@echo "[plume] Installing host test runner"
	mkdir -p $(TOOL_INSTALL)/kernel-testrunner
	cp $(RUNNER) $(TOOL_INSTALL)/kernel-testrunner/host-test-runner
