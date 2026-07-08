# EDK2 RiscVVirt UEFI firmware, fetched prebuilt (Alpine does not package it).
# Host-side test dependency only: QEMU loads it as pflash. Never enters sysroot.
# riscv64-only -- every step is a no-op on other architectures.

# Pinned by commit so the blob is immutable; the nightly "latest" URL is
# rebuilt continuously and must not be fetched directly.
EDK2_COMMIT  := 8c0de7688a7f49e7c0cc105b8244242b90c4005f
EDK2_URL     := https://raw.githubusercontent.com/retrage/edk2-nightly/$(EDK2_COMMIT)/bin
CODE_FD      := RELEASERISCV64_VIRT_CODE.fd
VARS_FD      := RELEASERISCV64_VIRT_VARS.fd
CODE_SHA256  := 7802a27ac8128d60227bf25d0982b702d9b1ab54b61d1bc0d8b82fdc587d5e4f
VARS_SHA256  := 8b634c1e6bd11607850b69111f6c4dbd1583dbbd1460dac72dbacbdc1a4a130a

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

# Download to a temp name and mv only after the checksum passes, so a
# truncated or tampered fetch is never mistaken for a cached-complete file.
pkg_get_source:
ifeq ($(ARCH),riscv64)
	@if [ ! -f "$(S)/$(CODE_FD)" ] || [ ! -f "$(S)/$(VARS_FD)" ]; then \
		echo "[plume] Fetching EDK2 RiscVVirt firmware ($(EDK2_COMMIT))..."; \
		mkdir -p $(S); \
		curl -fsSL -o $(S)/$(CODE_FD).tmp $(EDK2_URL)/$(CODE_FD) && \
		curl -fsSL -o $(S)/$(VARS_FD).tmp $(EDK2_URL)/$(VARS_FD) && \
		echo "$(CODE_SHA256)  $(S)/$(CODE_FD).tmp" | sha256sum -c - && \
		echo "$(VARS_SHA256)  $(S)/$(VARS_FD).tmp" | sha256sum -c - && \
		mv $(S)/$(CODE_FD).tmp $(S)/$(CODE_FD) && \
		mv $(S)/$(VARS_FD).tmp $(S)/$(VARS_FD); \
	fi
else
	@true
endif

pkg_configure:
	@true

pkg_build:
	@true

pkg_install:
ifeq ($(ARCH),riscv64)
	@echo "[plume] Installing $(CATEGORY)/$(P)"
	mkdir -p $(TOOL_INSTALL)/edk2-riscv
	cp $(S)/$(CODE_FD) $(TOOL_INSTALL)/edk2-riscv/RISCV_VIRT_CODE.fd
	cp $(S)/$(VARS_FD) $(TOOL_INSTALL)/edk2-riscv/RISCV_VIRT_VARS.fd
	# QEMU virt requires pflash images to fill the 32 MiB flash bank exactly.
	truncate -s 32M $(TOOL_INSTALL)/edk2-riscv/RISCV_VIRT_CODE.fd
	truncate -s 32M $(TOOL_INSTALL)/edk2-riscv/RISCV_VIRT_VARS.fd
else
	@true
endif
