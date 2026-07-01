# Host fuzz lane: libFuzzer over the pure demangler. A separate package from kernel-testrunner because
# the fuzzer/coverage instrumentation is its own build mode (ASan/fuzzer/TSan don't co-instrument).
# Build tool: stays on the host, not installed into the sysroot.

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

KSRC   := $(LIVE_SOURCES)
OBJDIR := $(BUILD_DIR)/obj/test/kernel-fuzz
FUZZER := $(OBJDIR)/fuzz-demangle

INC_KERNEL := -I $(KSRC)/includes/std -I $(KSRC)/includes

# Same amplifier dialect as the host runner, plus libFuzzer (brings coverage instrumentation and its
# own main()) and ASan/UBSan. The host is logic-authoritative, not the target ABI -- bare-metal codegen
# flags are dropped, matching kernel-testrunner.
SAN   := -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all
FLAGS := -std=c++20 -g -O1 -ffreestanding -fno-builtin -fno-exceptions -fno-rtti \
	-fno-omit-frame-pointer -fvisibility=hidden -fvisibility-inlines-hidden -fstrict-aliasing $(SAN)

SRCS := $(KSRC)/core/demangle.cpp $(KSRC)/tests/fuzz/demangle_fuzz.cpp
OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(basename $(notdir $(SRCS)))))

pkg_get_source:
	@true

pkg_configure:
	@true

pkg_build:
	@echo "[plume] Building demangle fuzzer"
	mkdir -p $(OBJDIR)
	@for src in $(SRCS); do \
		obj=$(OBJDIR)/$$(basename $$src .cpp).o; \
		echo "  CXX (fuzz) $$src"; \
		$(CXX) $(FLAGS) $(INC_KERNEL) -c $$src -o $$obj || exit 1; \
	done
	$(CXX) $(SAN) -fuse-ld=lld $(OBJS) -o $(FUZZER)

pkg_install:
	@echo "[plume] Installing demangle fuzzer"
	mkdir -p $(TOOL_INSTALL)/kernel-fuzz
	cp $(FUZZER) $(TOOL_INSTALL)/kernel-fuzz/fuzz-demangle
