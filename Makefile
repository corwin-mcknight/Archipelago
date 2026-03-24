BUILD_DIR := ${PWD}/build
KERNEL_SRC_DIR := ${PWD}/src/sys/kernel
KERNEL_INCLUDE_DIR := ${KERNEL_SRC_DIR}/includes
KERNEL_DOCS_DIR := ${KERNEL_SRC_DIR}/docs
ARCHIPELAGO_VERSION ?= $(shell git describe --tags --dirty --always 2>/dev/null || echo dev)
DOCS_DOXYFILE := ${KERNEL_DOCS_DIR}/Doxyfile
DOCS_OUTPUT_DIR := ${BUILD_DIR}/docs/kernel

# Tracked paths for incremental install/ISO rebuild.
KERNEL_OBJ_ELF     := $(BUILD_DIR)/obj/sys/kernel/kernel.elf
KERNEL_SYSROOT_ELF := $(BUILD_DIR)/sysroot/boot/kernel.elf
MEDIA_FILES        := $(shell find $(PWD)/media -type f)

.PHONY: all build kernel install run clean full-clean clangd docs test test-verbose format

all: build install run

build: kernel

kernel:
	mkdir -p ${BUILD_DIR}/obj/sys/kernel/
	cd src/sys/kernel && $(MAKE) BUILD_DIR=${BUILD_DIR}

# Copy kernel ELF into the sysroot only when it actually changes.
$(KERNEL_SYSROOT_ELF): $(KERNEL_OBJ_ELF)
	@mkdir -p $(BUILD_DIR)/sysroot/boot/
	@cp $< $@

# Build the ISO only when the sysroot ELF or Limine/media configs change.
$(BUILD_DIR)/image.iso: $(KERNEL_SYSROOT_ELF) $(MEDIA_FILES)
	@cd tools && ./fs-install-limine.sh

# install is a convenience alias; the real work is in the file targets above.
install: kernel $(BUILD_DIR)/image.iso

run:  install
	@clear
	@qemu-system-x86_64 --cdrom build/image.iso -serial stdio -m 128 -smp 1

debug: install
	qemu-system-x86_64 --cdrom build/image.iso -serial stdio -s  -S -m 128 -smp 4

clean:
	-@rm -r ${BUILD_DIR}/obj
	-@rm -r ${BUILD_DIR}/sysroot
	-@rm ${BUILD_DIR}/image.iso

full-clean:
	-@rm -r ${BUILD_DIR}
	-@mkdir -p ${BUILD_DIR}
	-@rm -r ${PWD}/.cache

# Regenerate compile_commands.json for clangd/IDE support.
# Uses clang's -MJ flag — forces full recompile of kernel sources (not ISO).
# Re-run after adding/removing source files or changing include paths.
clangd:
	@$(MAKE) -B build
	@python3 tools/merge-compile-commands.py $(BUILD_DIR) $(BUILD_DIR)/compile_commands.json

test: install
	@echo -e "\n\nRunning tests...\n"
	@python3 tools/test-harness.py $(TEST)

test-verbose: install
	@echo -e "\n\nRunning tests...\n"
	@python3 tools/test-harness.py --verbose $(TEST)

# Format all kernel source and header files with clang-format.
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
