.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

pkg_get_source pkg_configure:
	@true

pkg_build:
	mkdir -p $(OBJ_DIR)
	cd $(LIVE_SOURCES) && $(MAKE) -j$(MAKE_JOBS) BUILD_DIR=$(BUILD_DIR) OBJ_DIR=$(OBJ_DIR) SYSROOT=$(SYSROOT)

pkg_install:
	mkdir -p $(D)/usr/share/initrd/bootstrap $(D)/usr/share/initrd/data $(D)/usr/include/fileservice
	cp $(OBJ_DIR)/files.elf $(D)/usr/share/initrd/bootstrap/
	cp $(LIVE_SOURCES)/file_service.txt $(D)/usr/share/initrd/data/
	cp $(LIVE_SOURCES)/hello.txt $(D)/usr/share/initrd/
	cp $(LIVE_SOURCES)/include/fileservice/protocol.h $(D)/usr/include/fileservice/
