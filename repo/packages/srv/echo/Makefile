# The echo server, loaded from the boot image like init. Built in C against the installed public
# ABI headers and the lib/crt runtime, so it depends on both being in the sysroot first.
#
# No board facts are compiled in, so like init this builds once per architecture.

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

pkg_get_source:
	@true

pkg_configure:
	@true

pkg_build:
	@echo "[plume] Building $(CATEGORY)/$(P)"
	mkdir -p $(OBJ_DIR)
	cd $(LIVE_SOURCES) && $(MAKE) -j$(MAKE_JOBS) BUILD_DIR=$(BUILD_DIR) OBJ_DIR=$(OBJ_DIR) SYSROOT=$(SYSROOT)

pkg_install:
	@echo "[plume] Installing $(CATEGORY)/$(P)"
	mkdir -p $(D)/boot
	cp $(OBJ_DIR)/echo.elf $(D)/boot/echo.elf
