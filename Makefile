BUILD_DIR := ${PWD}/build

.PHONY: all build kernel install run clean clangd

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