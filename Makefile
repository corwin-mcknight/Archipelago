BUILD_DIR := ${PWD}/build
KERNEL_SRC_DIR := ${PWD}/src/sys/kernel
KERNEL_INCLUDE_DIR := ${KERNEL_SRC_DIR}/includes
KERNEL_DOCS_DIR := ${KERNEL_SRC_DIR}/docs
ARCHIPELAGO_VERSION ?= $(shell git describe --tags --dirty --always 2>/dev/null || echo dev)
DOCS_DOXYFILE := ${KERNEL_DOCS_DIR}/Doxyfile
DOCS_OUTPUT_DIR := ${BUILD_DIR}/docs/kernel

.PHONY: all build kernel install run clean clangd docs

all: build install run

build: kernel

kernel:
	mkdir -p ${BUILD_DIR}/obj/sys/kernel/
	cd src/sys/kernel && make BUILD_DIR=${BUILD_DIR}

install: kernel
	@mkdir -p ${BUILD_DIR}/sysroot/boot/
	@ cd src/sys/kernel && make install BUILD_DIR=${BUILD_DIR}
	@cd tools && ./fs-install-limine.sh

run: 
	@clear
	@qemu-system-x86_64 --cdrom build/image.iso -serial stdio -m 64

debug: install
	qemu-system-x86_64 --cdrom build/image.iso -serial stdio -s  -S -m 64

clean:
	-@rm -r ${BUILD_DIR}/obj
	-@rm -r ${BUILD_DIR}/sysroot
	-@rm ${BUILD_DIR}/image.iso

full-clean: 
	-@rm -r ${BUILD_DIR}
	-@mkdir -p ${BUILD_DIR}
	-@rm -r ${PWD}/.cache

clangd: clean
	@mkdir -p ${BUILD_DIR}
	@bear --append --output ${BUILD_DIR}/compile_commands.json -- make build


test: install
	@echo -e "\n\nRunning tests...\n"
	@python3 tools/test-harness.py 

test-verbose: install
	@echo -e "\n\nRunning tests...\n"
	@python3 tools/test-harness.py --verbose
docs:
	@command -v doxygen >/dev/null 2>&1 || { echo 'doxygen not found. Install doxygen to build kernel docs.' >&2; exit 1; }
	@mkdir -p ${DOCS_OUTPUT_DIR}
	@ARCHIPELAGO_VERSION="${ARCHIPELAGO_VERSION}" \
	KERNEL_SRC_DIR="${KERNEL_SRC_DIR}" \
	KERNEL_INCLUDE_DIR="${KERNEL_INCLUDE_DIR}" \
	KERNEL_DOCS_DIR="${KERNEL_DOCS_DIR}" \
	DOCS_OUTPUT_DIR="${DOCS_OUTPUT_DIR}" doxygen ${DOCS_DOXYFILE}
	@echo 'Kernel documentation available in ${DOCS_OUTPUT_DIR}/html'
