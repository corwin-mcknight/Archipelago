# The user-program C runtime: _start, syscall stubs, the user linker script, and the sys.h
# surface, installed as a static archive next to the public ABI headers. Deliberately not a libc.
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
	mkdir -p $(D)/usr/lib $(D)/usr/include
	cp $(OBJ_DIR)/libcrt.a $(D)/usr/lib/libcrt.a
	cp $(LIVE_SOURCES)/user.ld $(D)/usr/lib/user.ld
	cp $(LIVE_SOURCES)/include/sys.h $(D)/usr/include/sys.h
