# Host fuzz lane: libFuzzer over pure kernel parsers (the demangler and the ktl::format brace parser).
# A separate package from kernel-testrunner because the fuzzer/coverage instrumentation is its own
# build mode (ASan/fuzzer/TSan don't co-instrument). Build tool: stays on the host, not installed
# into the sysroot.

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

KSRC   := $(LIVE_SOURCES)
OBJDIR := $(BUILD_DIR)/obj/test/kernel-fuzz
FUZZ_DEMANGLE := $(OBJDIR)/fuzz-demangle
FUZZ_FMT      := $(OBJDIR)/fuzz-fmt

INC_KERNEL := -I $(KSRC)/includes/std -I $(KSRC)/includes

# Same amplifier dialect as the host runner, plus libFuzzer (brings coverage instrumentation and its
# own main()) and ASan/UBSan. The host is logic-authoritative, not the target ABI -- bare-metal codegen
# flags are dropped, matching kernel-testrunner.
SAN   := -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all
FLAGS := -std=c++20 -g -O1 -ffreestanding -fno-builtin -fno-exceptions -fno-rtti \
	-fno-omit-frame-pointer -fvisibility=hidden -fvisibility-inlines-hidden -fstrict-aliasing $(SAN)

DEMANGLE_SRCS := $(KSRC)/core/demangle.cpp $(KSRC)/tests/fuzz/demangle_fuzz.cpp
DEMANGLE_OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(basename $(notdir $(DEMANGLE_SRCS)))))

pkg_get_source:
	@true

pkg_configure:
	@true

pkg_build:
	@echo "[plume] Building fuzzers (demangle, fmt)"
	mkdir -p $(OBJDIR)
	@for src in $(DEMANGLE_SRCS); do \
		obj=$(OBJDIR)/$$(basename $$src .cpp).o; \
		echo "  CXX (fuzz) $$src"; \
		$(CXX) $(FLAGS) $(INC_KERNEL) -c $$src -o $$obj || exit 1; \
	done
	$(CXX) $(SAN) -fuse-ld=lld $(DEMANGLE_OBJS) -o $(FUZZ_DEMANGLE)
	@echo "  CXX (fuzz) $(KSRC)/tests/fuzz/fmt_fuzz.cpp"
	$(CXX) $(FLAGS) $(INC_KERNEL) -c $(KSRC)/tests/fuzz/fmt_fuzz.cpp -o $(OBJDIR)/fmt_fuzz.o
	$(CXX) $(FLAGS) $(INC_KERNEL) -c $(KSRC)/std/stdlib.cpp -o $(OBJDIR)/stdlib.o
	$(CXX) $(SAN) -fuse-ld=lld $(OBJDIR)/fmt_fuzz.o $(OBJDIR)/stdlib.o -o $(FUZZ_FMT)

pkg_install:
	@echo "[plume] Installing fuzzers"
	mkdir -p $(TOOL_INSTALL)/kernel-fuzz
	cp $(FUZZ_DEMANGLE) $(TOOL_INSTALL)/kernel-fuzz/fuzz-demangle
	cp $(FUZZ_FMT) $(TOOL_INSTALL)/kernel-fuzz/fuzz-fmt
