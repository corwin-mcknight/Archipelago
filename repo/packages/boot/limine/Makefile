.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

LIMINE_TOOLS := $(TOOLS_PATH)/limine

pkg_get_source:
	@true

pkg_configure:
	@true

pkg_build:
	@true

pkg_install:
	@echo "[plume] Installing $(CATEGORY)/$(P)"
	mkdir -p $(D)/boot
	cp $(LIMINE_TOOLS)/limine-bios-cd.bin $(D)/boot/
	cp $(LIMINE_TOOLS)/limine-uefi-cd.bin $(D)/boot/
	cp $(LIMINE_TOOLS)/limine-bios.sys $(D)/boot/
