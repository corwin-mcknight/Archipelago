PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DEV := "$(PROJECT_ROOT)/tools/dev"
PYTHON ?= python3
PLUME := $(DEV) $(PYTHON) -m plume
TARGET_ARGS = $(if $(ARCH),--arch $(ARCH))
QEMU_TEST_ARGS = $(if $(QEMU_JOBS),--qemu-jobs $(QEMU_JOBS)) \
	$(if $(QEMU_BOOT_TIMEOUT),--qemu-boot-timeout $(QEMU_BOOT_TIMEOUT)) \
	$(if $(QEMU_COMMAND_TIMEOUT),--qemu-command-timeout $(QEMU_COMMAND_TIMEOUT)) \
	$(if $(QEMU_TEST_TIMEOUT),--qemu-test-timeout $(QEMU_TEST_TIMEOUT)) \
	$(if $(QEMU_RETRIES),--qemu-retries $(QEMU_RETRIES))

KERNEL_SRC_DIR     := $(PROJECT_ROOT)/src/sys/kernel
KERNEL_INCLUDE_DIR := ${KERNEL_SRC_DIR}/includes
KERNEL_DOCS_DIR    := ${KERNEL_SRC_DIR}/docs
ARCHIPELAGO_VERSION ?= $(shell git describe --tags --dirty --always 2>/dev/null || echo dev)
DOCS_DOXYFILE      := ${KERNEL_SRC_DIR}/Doxyfile
DOCS_OUTPUT_DIR    := $(PROJECT_ROOT)/build/docs/kernel

.PHONY: all setup doctor selfcheck build install test test-verbose uboot-test netboot board-test console host-test host-coverage host-fuzz host-tsan shell clean full-clean clangd format docs run debug

all: install

setup:
	@"$(PROJECT_ROOT)/tools/setup"

doctor:
	@$(DEV) $(PYTHON) -B tools/doctor.py $(TARGET_ARGS)

selfcheck:
	@$(DEV) $(PYTHON) -m plume.selfcheck
	@$(DEV) $(PYTHON) tools/environment-selfcheck.py

build:
	@$(PLUME) build $(TARGET_ARGS)

install: build
	@$(PLUME) image $(TARGET_ARGS)

test:
	@$(PLUME) test $(TARGET_ARGS) $(QEMU_TEST_ARGS) $(TEST)

test-verbose:
	@$(PLUME) test $(TARGET_ARGS) $(QEMU_TEST_ARGS) --verbose $(TEST)

# Rebuild the jh7110 image and refresh the TFTP root the board netboots from.
netboot:
	@$(PLUME) build --arch riscv64^jh7110
	@$(PLUME) image --arch riscv64^jh7110
	@$(DEV) $(PYTHON) tools/netboot.py

# Run kernel tests on the real board over serial: all tests, or TEST=<name>.
# Non-crash tests batch onto shared boots; FRESH=1 reboots before every test.
# Requires the board netbooted from the current build (`make netboot` + reboot).
board-test:
	@$(DEV) $(PYTHON) tools/board_test.py $(if $(TEST),--test $(TEST)) $(if $(FRESH),--fresh)

# Interactive serial console on the board, shared with automation through the
# mux (started here if not already running). Ctrl-] detaches.
console:
	@$(DEV) $(PYTHON) -c "import socket; socket.create_connection(('localhost', 5556), 1)" 2>/dev/null || \
		{ nohup $(DEV) $(PYTHON) tools/serial_mux.py >/tmp/serial-mux.log 2>&1 & sleep 1; }
	@echo "board serial console -- Ctrl-] to detach"
	@$(DEV) socat -,rawer,escape=0x1d TCP:localhost:5556

# Boot-chain smoke test (riscv64): OpenSBI -> U-Boot EFI -> Limine -> kernel
# in QEMU, the same chain real boards use from an SD card.
uboot-test:
	@$(PLUME) build --arch riscv64
	@$(PLUME) uboot-test --arch riscv64

host-test:
	@$(PLUME) build $(TARGET_ARGS) test/kernel-testrunner
	@$(DEV) $(PYTHON) tools/host-test.py $(TEST)

host-coverage:
	@$(DEV) $(PYTHON) tools/host-coverage.py $(if $(COV_MIN),--min $(COV_MIN),) $(TEST)

# Periodic/on-demand lane, not the inner loop. FUZZ_TIME caps wall-clock (default 30s); crashes land
# under build/host-fuzz/ as repro files. No seed corpus/dict -- coverage feedback finds the
# 2-byte "_Z" prefix in well under a second; add a dict if a deeper target ever needs steering.
# LSan is disabled: it is unreliable on musl and the target allocates nothing, so the at-exit check
# only ever flags libFuzzer's own retained corpus state.
host-fuzz:
	@$(PLUME) build $(TARGET_ARGS) test/kernel-fuzz
	@mkdir -p build/host-fuzz/$(if $(FUZZ),$(FUZZ),demangle)/corpus
	@ASAN_OPTIONS=detect_leaks=0 $(DEV) build/tools/kernel-fuzz/fuzz-$(if $(FUZZ),$(FUZZ),demangle) \
		-artifact_prefix=build/host-fuzz/$(if $(FUZZ),$(FUZZ),demangle)/ \
		-max_total_time=$(if $(FUZZ_TIME),$(FUZZ_TIME),30) build/host-fuzz/$(if $(FUZZ),$(FUZZ),demangle)/corpus

# Periodic/on-demand lane: real-thread stress over lock-free KTL data structures under TSan. A TSan
# report (data race / missing synchronization) aborts with nonzero exit.
host-tsan:
	@$(PLUME) build $(TARGET_ARGS) test/kernel-tsan
	@$(DEV) build/tools/kernel-tsan/tsan-atomic
	@$(DEV) build/tools/kernel-tsan/tsan-log-ring

shell: install
	@$(PLUME) run $(TARGET_ARGS) --no-display

clean:
	@$(PLUME) clean $(TARGET_ARGS)

full-clean:
	-@rm -rf "$(PROJECT_ROOT)/build"
	@mkdir -p "$(PROJECT_ROOT)/build"
	-@rm -rf "$(PROJECT_ROOT)/.cache"

clangd:
	@$(PLUME) clangd $(TARGET_ARGS)

format:
	@echo "Formatting source files..."
	@find "$(KERNEL_SRC_DIR)" \( -name '*.cpp' -o -name '*.h' \) -print0 | $(DEV) xargs -0 clang-format -i
	@echo "Done."

docs:
	@$(DEV) sh -c 'command -v doxygen >/dev/null 2>&1' || { echo 'doxygen not found. Install doxygen to build kernel docs.' >&2; exit 1; }
	@mkdir -p "${DOCS_OUTPUT_DIR}"
	@ARCHIPELAGO_VERSION="${ARCHIPELAGO_VERSION}" \
	KERNEL_SRC_DIR="${KERNEL_SRC_DIR}" \
	KERNEL_INCLUDE_DIR="${KERNEL_INCLUDE_DIR}" \
	KERNEL_DOCS_DIR="${KERNEL_DOCS_DIR}" \
	DOCS_OUTPUT_DIR="${DOCS_OUTPUT_DIR}" $(DEV) doxygen "${DOCS_DOXYFILE}"
	@echo 'Kernel documentation available in ${DOCS_OUTPUT_DIR}/html'

run: install
	@$(PLUME) run $(TARGET_ARGS)

debug: install
	@$(PLUME) run $(TARGET_ARGS) --debug
