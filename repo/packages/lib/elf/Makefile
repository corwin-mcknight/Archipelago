.PHONY: pkg_get_source pkg_configure pkg_build pkg_install
pkg_get_source pkg_configure:
	@true
pkg_build:
	cd $(LIVE_SOURCES) && $(MAKE) -j$(MAKE_JOBS) BUILD_DIR=$(BUILD_DIR) OBJ_DIR=$(OBJ_DIR) SYSROOT=$(SYSROOT)
pkg_install:
	mkdir -p $(D)/usr/lib $(D)/usr/include
	cp $(OBJ_DIR)/libelf.a $(D)/usr/lib/libelf.a
	cp -r $(LIVE_SOURCES)/include/elf $(D)/usr/include/
