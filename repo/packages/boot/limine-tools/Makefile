LIMINE_REPO  := https://github.com/limine-bootloader/limine.git
# Pinned commit on v10.x-binary: a clean build always fetches the same
# bootloader; bump deliberately, never by re-cloning a moving branch.
LIMINE_COMMIT := 7a9013f305de0bea1f6310e76e7baba30499fef0

.PHONY: pkg_get_source pkg_configure pkg_build pkg_install

pkg_get_source:
	@if [ ! -d "$(S)/.git" ]; then \
		echo "[plume] Fetching Limine ($(LIMINE_COMMIT))..."; \
		git init -q $(S); \
		git -C $(S) remote add origin $(LIMINE_REPO); \
		git -C $(S) fetch -q --depth=1 origin $(LIMINE_COMMIT); \
		git -C $(S) checkout -q FETCH_HEAD; \
	fi

pkg_configure:
	@true

pkg_build:
	@echo "[plume] Building $(CATEGORY)/$(P)"
	$(MAKE) -C $(S)

pkg_install:
	@echo "[plume] Installing $(CATEGORY)/$(P)"
	mkdir -p $(TOOL_INSTALL)/limine-tools
	cp $(S)/limine $(TOOL_INSTALL)/limine-tools/
	cp $(S)/limine-bios-cd.bin $(TOOL_INSTALL)/limine-tools/
	cp $(S)/limine-uefi-cd.bin $(TOOL_INSTALL)/limine-tools/
	cp $(S)/limine-bios.sys $(TOOL_INSTALL)/limine-tools/
	cp $(S)/BOOTRISCV64.EFI $(TOOL_INSTALL)/limine-tools/
