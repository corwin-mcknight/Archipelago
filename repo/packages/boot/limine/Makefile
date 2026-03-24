LIMINE_REPO  := https://github.com/limine-bootloader/limine.git
LIMINE_BRANCH := v10.x-binary

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

pkg_get_source:
	@if [ ! -d "$(S)/.git" ]; then \
		echo "[plume] Cloning Limine..."; \
		git clone $(LIMINE_REPO) --branch=$(LIMINE_BRANCH) --depth=1 $(S); \
	fi

pkg_configure:
	@true

pkg_build:
	@echo "[plume] Building $(CATEGORY)/$(P)"
	$(MAKE) -C $(S)

pkg_install:
	@echo "[plume] Installing $(CATEGORY)/$(P)"
	mkdir -p $(TOOL_INSTALL)/limine
	cp $(S)/limine $(TOOL_INSTALL)/limine/
	cp $(S)/limine-bios-cd.bin $(TOOL_INSTALL)/limine/
	cp $(S)/limine-uefi-cd.bin $(TOOL_INSTALL)/limine/
	cp $(S)/limine-bios.sys $(TOOL_INSTALL)/limine/
