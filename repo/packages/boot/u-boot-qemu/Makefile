# Mainline U-Boot built for QEMU's riscv64 virt machine (S-mode payload under
# OpenSBI), fetched prebuilt from Debian (Alpine does not package it for
# riscv64). Host-side test dependency for `plume uboot-test`: QEMU loads it as
# the -kernel payload. Never enters sysroot. riscv64-only -- every step is a
# no-op on other architectures.

# Pinned by exact package version so the blob is immutable.
UBOOT_DEB    := u-boot-qemu_2025.01-3.2_all.deb
UBOOT_URL    := https://deb.debian.org/debian/pool/main/u/u-boot/$(UBOOT_DEB)
UBOOT_SHA256 := 27cef6cc201f578404564766818edafcda0cc8e227cc5a6d349f315b120a26be
UBOOT_BIN    := usr/lib/u-boot/qemu-riscv64_smode/u-boot.bin

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

# Download to a temp name and mv only after the checksum passes, so a
# truncated or tampered fetch is never mistaken for a cached-complete file.
pkg_get_source:
ifeq ($(ARCH),riscv64)
	@if [ ! -f "$(S)/$(UBOOT_DEB)" ]; then \
		echo "[plume] Fetching U-Boot QEMU binaries ($(UBOOT_DEB))..."; \
		mkdir -p $(S); \
		curl -fsSL -o $(S)/$(UBOOT_DEB).tmp $(UBOOT_URL) && \
		echo "$(UBOOT_SHA256)  $(S)/$(UBOOT_DEB).tmp" | sha256sum -c - && \
		mv $(S)/$(UBOOT_DEB).tmp $(S)/$(UBOOT_DEB); \
	fi
else
	@true
endif

pkg_configure:
	@true

# A .deb is an ar archive wrapping data.tar.xz; pull just the S-mode binary.
pkg_build:
ifeq ($(ARCH),riscv64)
	cd $(S) && ar p $(UBOOT_DEB) data.tar.xz | tar -xJ ./$(UBOOT_BIN)
else
	@true
endif

pkg_install:
ifeq ($(ARCH),riscv64)
	@echo "[plume] Installing $(CATEGORY)/$(PN)"
	mkdir -p $(TOOL_INSTALL)/u-boot-qemu
	cp $(S)/$(UBOOT_BIN) $(TOOL_INSTALL)/u-boot-qemu/u-boot.bin
else
	@true
endif
