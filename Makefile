PLUME := python3 -m plume

KERNEL_SRC_DIR     := ${PWD}/src/sys/kernel
KERNEL_INCLUDE_DIR := ${KERNEL_SRC_DIR}/includes
KERNEL_DOCS_DIR    := ${KERNEL_SRC_DIR}/docs
ARCHIPELAGO_VERSION ?= $(shell git describe --tags --dirty --always 2>/dev/null || echo dev)
DOCS_DOXYFILE      := ${KERNEL_SRC_DIR}/Doxyfile
DOCS_OUTPUT_DIR    := ${PWD}/build/docs/kernel

.PHONY: all build install test test-verbose host-test host-coverage host-fuzz host-tsan shell clean full-clean clangd format docs

all: install

build:
	@$(PLUME) build @system

rebuild:
	@$(PLUME) rebuild @system

install: build
	@$(PLUME) install @system
	@$(PLUME) image

test:
	@$(PLUME) test $(TEST)

test-verbose:
	@$(PLUME) test --verbose $(TEST)

host-test:
	@$(PLUME) build test/kernel-testrunner
	@python3 tools/host-test.py $(TEST)

host-coverage:
	@python3 tools/host-coverage.py $(if $(COV_MIN),--min $(COV_MIN),) $(TEST)

# Periodic/on-demand lane, not the inner loop. FUZZ_TIME caps wall-clock (default 30s); crashes land
# under build/host-fuzz/ as repro files. ponytail: no seed corpus/dict -- coverage feedback finds the
# 2-byte "_Z" prefix in well under a second; add a dict if a deeper target ever needs steering.
# LSan is disabled: it is unreliable on musl and the target allocates nothing, so the at-exit check
# only ever flags libFuzzer's own retained corpus state.
host-fuzz:
	@$(PLUME) build test/kernel-fuzz
	@mkdir -p build/host-fuzz/$(if $(FUZZ),$(FUZZ),demangle)/corpus
	@ASAN_OPTIONS=detect_leaks=0 build/tools/kernel-fuzz/fuzz-$(if $(FUZZ),$(FUZZ),demangle) \
		-artifact_prefix=build/host-fuzz/$(if $(FUZZ),$(FUZZ),demangle)/ \
		-max_total_time=$(if $(FUZZ_TIME),$(FUZZ_TIME),30) build/host-fuzz/$(if $(FUZZ),$(FUZZ),demangle)/corpus

# Periodic/on-demand lane: real-thread stress over lock-free KTL data structures under TSan. A TSan
# report (data race / missing synchronization) aborts with nonzero exit.
host-tsan:
	@$(PLUME) build test/kernel-tsan
	@build/tools/kernel-tsan/tsan-atomic
	@build/tools/kernel-tsan/tsan-log-ring

shell: install
	@qemu-system-x86_64 --cdrom build/image.iso -serial stdio -display none -m 128 -smp 1

clean:
	@$(PLUME) clean

full-clean:
	-@rm -rf ${PWD}/build
	@mkdir -p ${PWD}/build
	-@rm -rf ${PWD}/.cache

clangd:
	@$(PLUME) clangd

format:
	@echo "Formatting source files..."
	@find $(KERNEL_SRC_DIR) \( -name '*.cpp' -o -name '*.h' \) | xargs clang-format -i
	@echo "Done."

docs:
	@command -v doxygen >/dev/null 2>&1 || { echo 'doxygen not found. Install doxygen to build kernel docs.' >&2; exit 1; }
	@mkdir -p ${DOCS_OUTPUT_DIR}
	@ARCHIPELAGO_VERSION="${ARCHIPELAGO_VERSION}" \
	KERNEL_SRC_DIR="${KERNEL_SRC_DIR}" \
	KERNEL_INCLUDE_DIR="${KERNEL_INCLUDE_DIR}" \
	KERNEL_DOCS_DIR="${KERNEL_DOCS_DIR}" \
	DOCS_OUTPUT_DIR="${DOCS_OUTPUT_DIR}" doxygen ${DOCS_DOXYFILE}
	@echo 'Kernel documentation available in ${DOCS_OUTPUT_DIR}/html'

run: install
	@qemu-system-x86_64 --cdrom build/image.iso -serial stdio -m 128 -smp 1

debug: install
	qemu-system-x86_64 --cdrom build/image.iso -serial stdio -s  -S -m 128 -smp 4
