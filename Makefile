PLUME := python3 -m plume

KERNEL_SRC_DIR     := ${PWD}/src/sys/kernel
KERNEL_INCLUDE_DIR := ${KERNEL_SRC_DIR}/includes
KERNEL_DOCS_DIR    := ${KERNEL_SRC_DIR}/docs
ARCHIPELAGO_VERSION ?= $(shell git describe --tags --dirty --always 2>/dev/null || echo dev)
DOCS_DOXYFILE      := ${KERNEL_SRC_DIR}/Doxyfile
DOCS_OUTPUT_DIR    := ${PWD}/build/docs/kernel

.PHONY: all build install test test-verbose shell clean full-clean clangd format docs

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
