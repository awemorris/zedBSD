TLS_FIXTURES ?= plan/ws022-elf-tls/temp/q128-fixtures
# WS022 TLS test-only amd64 probe/image rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ifneq ($(ZEDBSD_PLATFORM_DIR),amd64)
$(error WS006 IN-T12 requires an amd64/PC-AT configuration)
endif

WS022_P002_PROBE_SOURCE := \
	plan/ws022-elf-tls/tests/tls-loader-probe.c
WS022_P002_PROBE_OBJECT := \
	$(BUILD)/user64/plan/ws022-elf-tls/tests/tls-loader-probe.o
WS022_P002_PROBE := $(BUILD)/tests/tls-loader-probe
WS022_P002_UFS := $(ARCH_IMAGE_DIR)/amd64-ws022-p002.ufs
WS022_P002_IMAGE := $(BUILD)/ws022-p002-hdd-image.img

$(WS022_P002_PROBE_OBJECT): $(WS022_P002_PROBE_SOURCE) $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(WS022_P002_PROBE): $(AMD64_USER_LIBC_OBJS) \
	$(WS022_P002_PROBE_OBJECT) $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS022_P002_PROBE_OBJECT) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) \
		--machine amd64 $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS022_P002_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS022_P002_PROBE) $(wildcard $(TLS_FIXTURES)/amd64*.elf) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) \
	--file /usr/bin/tls-loader-probe=$(WS022_P002_PROBE) \
	$(foreach f,$(wildcard $(TLS_FIXTURES)/amd64*.elf),--file /usr/share/tls/$(notdir $(f))=$(f))))

$(WS022_P002_IMAGE): $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2-chain.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(WS022_P002_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/zedbsd.cfg \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct \
		--backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force \
		--machine pcat --layout hybrid \
		--checker platform/amd64/tools/check-amd64-gpt-image.noct \
		--checker-runner $(NOCT) \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2-chain.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		--kernel $(BUILD)/vmunix \
		--bootx64 $(BUILD)/uefi/BOOTX64.EFI \
		--zedbsd-config platform/amd64/zedbsd.cfg \
		--arch-profile amd64 --arch-image $(WS022_P002_UFS) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@

.PHONY: ws022-p002-qemu-image
ws022-p002-qemu-image: $(WS022_P002_IMAGE)
