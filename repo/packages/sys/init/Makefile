# The boot-only init image, loaded through the kernel's fixed-layout bootstrap path. Built against the
# installed public ABI headers, so it depends on sys/kernel-headers being in the sysroot first.
#
# No board facts are compiled in, so unlike the kernel this builds once per architecture.

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

pkg_get_source:
	@true

pkg_configure:
	@true

pkg_build:
	@echo "[plume] Building $(CATEGORY)/$(PN)"
	mkdir -p $(OBJ_DIR)
	cd $(LIVE_SOURCES) && $(MAKE) -j$(MAKE_JOBS) BUILD_DIR=$(BUILD_DIR) OBJ_DIR=$(OBJ_DIR) SYSROOT=$(SYSROOT)

pkg_install:
	@echo "[plume] Installing $(CATEGORY)/$(PN)"
	mkdir -p $(D)/boot
	# Retire the previous package output when upgrading an existing staging tree.
	rm -f $(D)/boot/$(PN).elf
	cp $(OBJ_DIR)/$(PN).bin $(D)/boot/$(PN).bin
