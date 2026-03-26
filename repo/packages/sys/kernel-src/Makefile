.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

pkg_get_source:
	@true

pkg_configure:
	@true

pkg_build:
	@echo "[plume] Archiving kernel sources"
	tar cJf $(WORKDIR)/kernel.tar.xz -C $(LIVE_SOURCES)/.. kernel

pkg_install:
	@echo "[plume] Installing $(CATEGORY)/$(P)"
	mkdir -p $(D)/usr/src
	cp $(WORKDIR)/kernel.tar.xz $(D)/usr/src/kernel.tar.xz
