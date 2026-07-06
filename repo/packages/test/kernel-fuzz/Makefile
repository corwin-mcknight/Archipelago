# Host fuzz lane: libFuzzer over pure kernel parsers/serializers (demangler, ktl::format brace parser,
# JSON string escaper, atoi). A separate package from kernel-testrunner because the fuzzer/coverage
# instrumentation is its own build mode (ASan/fuzzer/TSan don't co-instrument). Build tool: stays on
# the host, not installed into the sysroot.

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

KSRC   := $(LIVE_SOURCES)
OBJDIR := $(BUILD_DIR)/obj/test/kernel-fuzz
FUZZ_DEMANGLE := $(OBJDIR)/fuzz-demangle
FUZZ_FMT      := $(OBJDIR)/fuzz-fmt
FUZZ_JSON     := $(OBJDIR)/fuzz-json
FUZZ_ATOI     := $(OBJDIR)/fuzz-atoi
FUZZ_ELF      := $(OBJDIR)/fuzz-elf
FUZZ_RBTREE   := $(OBJDIR)/fuzz-rbtree

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
	@echo "[plume] Building fuzzers (demangle, fmt, json, atoi)"
	mkdir -p $(OBJDIR)
	@for src in $(DEMANGLE_SRCS); do \
		obj=$(OBJDIR)/$$(basename $$src .cpp).o; \
		echo "  CXX (fuzz) $$src"; \
		$(CXX) $(FLAGS) $(INC_KERNEL) -c $$src -o $$obj || exit 1; \
	done
	$(CXX) $(SAN) -fuse-ld=lld $(DEMANGLE_OBJS) -o $(FUZZ_DEMANGLE)
	# stdlib.o (atoi/itoa) is shared by the fmt and atoi fuzzers.
	$(CXX) $(FLAGS) $(INC_KERNEL) -c $(KSRC)/std/stdlib.cpp -o $(OBJDIR)/stdlib.o
	@echo "  CXX (fuzz) $(KSRC)/tests/fuzz/fmt_fuzz.cpp"
	$(CXX) $(FLAGS) $(INC_KERNEL) -c $(KSRC)/tests/fuzz/fmt_fuzz.cpp -o $(OBJDIR)/fmt_fuzz.o
	$(CXX) $(SAN) -fuse-ld=lld $(OBJDIR)/fmt_fuzz.o $(OBJDIR)/stdlib.o -o $(FUZZ_FMT)
	@echo "  CXX (fuzz) $(KSRC)/tests/fuzz/json_escape_fuzz.cpp"
	$(CXX) $(FLAGS) $(INC_KERNEL) -c $(KSRC)/tests/fuzz/json_escape_fuzz.cpp -o $(OBJDIR)/json_escape_fuzz.o
	$(CXX) $(SAN) -fuse-ld=lld $(OBJDIR)/json_escape_fuzz.o -o $(FUZZ_JSON)
	@echo "  CXX (fuzz) $(KSRC)/tests/fuzz/atoi_fuzz.cpp"
	$(CXX) $(FLAGS) $(INC_KERNEL) -c $(KSRC)/tests/fuzz/atoi_fuzz.cpp -o $(OBJDIR)/atoi_fuzz.o
	$(CXX) $(SAN) -fuse-ld=lld $(OBJDIR)/atoi_fuzz.o $(OBJDIR)/stdlib.o -o $(FUZZ_ATOI)
	# ELF parser lives in symbols.cpp (pure logic, no hardware deps); link it in for the parser under test.
	@echo "  CXX (fuzz) $(KSRC)/tests/fuzz/elf_symbols_fuzz.cpp"
	$(CXX) $(FLAGS) $(INC_KERNEL) -c $(KSRC)/tests/fuzz/elf_symbols_fuzz.cpp -o $(OBJDIR)/elf_symbols_fuzz.o
	$(CXX) $(FLAGS) $(INC_KERNEL) -c $(KSRC)/core/symbols.cpp -o $(OBJDIR)/symbols.o
	$(CXX) $(SAN) -fuse-ld=lld $(OBJDIR)/elf_symbols_fuzz.o $(OBJDIR)/symbols.o -o $(FUZZ_ELF)
	# rb_tree is header-only (intrusive, no allocation), so the fuzzer is self-contained.
	@echo "  CXX (fuzz) $(KSRC)/tests/fuzz/rb_tree_fuzz.cpp"
	$(CXX) $(FLAGS) $(INC_KERNEL) -c $(KSRC)/tests/fuzz/rb_tree_fuzz.cpp -o $(OBJDIR)/rb_tree_fuzz.o
	$(CXX) $(SAN) -fuse-ld=lld $(OBJDIR)/rb_tree_fuzz.o -o $(FUZZ_RBTREE)

pkg_install:
	@echo "[plume] Installing fuzzers"
	mkdir -p $(TOOL_INSTALL)/kernel-fuzz
	cp $(FUZZ_DEMANGLE) $(TOOL_INSTALL)/kernel-fuzz/fuzz-demangle
	cp $(FUZZ_FMT) $(TOOL_INSTALL)/kernel-fuzz/fuzz-fmt
	cp $(FUZZ_JSON) $(TOOL_INSTALL)/kernel-fuzz/fuzz-json
	cp $(FUZZ_ATOI) $(TOOL_INSTALL)/kernel-fuzz/fuzz-atoi
	cp $(FUZZ_ELF) $(TOOL_INSTALL)/kernel-fuzz/fuzz-elf
	cp $(FUZZ_RBTREE) $(TOOL_INSTALL)/kernel-fuzz/fuzz-rbtree
