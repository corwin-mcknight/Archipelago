.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

LIMINE_TOOLS := $(TOOLS_PATH)/limine-tools

pkg_get_source:
	@true

pkg_configure:
	@true

pkg_build:
	@true

pkg_install:
	@echo "[plume] Installing $(CATEGORY)/$(P) for $(ARCH)"
	mkdir -p $(D)/boot
ifeq ($(ARCH),riscv64)
	# riscv64 has no BIOS: the only boot artifact is an El Torito EFI system
	# partition image carrying Limine's riscv64 UEFI executable. Upstream's
	# limine-uefi-cd.bin only bundles the x86 executables, so build our own.
	rm -f $(D)/boot/limine-uefi-cd.bin
	truncate -s 2880K $(D)/boot/limine-uefi-cd.bin
	# mformat, not mkfs.vfat: busybox mkfs.vfat always writes FAT32, which is
	# out of spec at this size and rejected by EDK2's FAT driver. -f 2880 is
	# the standard 2.88M FAT12 geometry.
	mformat -i $(D)/boot/limine-uefi-cd.bin -f 2880 ::
	mmd -i $(D)/boot/limine-uefi-cd.bin ::/EFI ::/EFI/BOOT
	mcopy -i $(D)/boot/limine-uefi-cd.bin $(LIMINE_TOOLS)/BOOTRISCV64.EFI ::/EFI/BOOT/
else
	cp $(LIMINE_TOOLS)/limine-bios-cd.bin $(D)/boot/
	cp $(LIMINE_TOOLS)/limine-uefi-cd.bin $(D)/boot/
	cp $(LIMINE_TOOLS)/limine-bios.sys $(D)/boot/
endif
