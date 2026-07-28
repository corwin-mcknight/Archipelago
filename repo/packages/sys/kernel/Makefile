.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

pkg_get_source:
	@true

pkg_configure:
	@true

# OBJ_DIR comes from plume and carries the board variant, so boards on one
# arch compile into separate object trees instead of clobbering each other.
pkg_build:
	@echo "[plume] Building $(CATEGORY)/$(P)"
	mkdir -p $(OBJ_DIR)
	cd $(LIVE_SOURCES) && $(MAKE) -j$(MAKE_JOBS) BUILD_DIR=$(BUILD_DIR) OBJ_DIR=$(OBJ_DIR)

pkg_install:
	@echo "[plume] Installing $(CATEGORY)/$(P)"
	mkdir -p $(D)/boot
	cp $(OBJ_DIR)/kernel.elf $(D)/boot/kernel.elf
