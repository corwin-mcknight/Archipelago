# Public kernel headers: the user/kernel contract, installed to /usr/include so user programs build
# against one copy of it instead of duplicating syscall numbers. Everything under includes/abi/ is
# public by definition; everything else in the kernel tree is not.

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

pkg_get_source:
	@true

pkg_configure:
	@true

pkg_build:
	@true

pkg_install:
	@echo "[plume] Installing $(CATEGORY)/$(PN)"
	mkdir -p $(D)/usr/include
	cp -r $(LIVE_SOURCES)/includes/abi $(D)/usr/include/
