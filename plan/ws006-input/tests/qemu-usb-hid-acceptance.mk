# WS006 IN-T41 test-only guest probe and USB-root image rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ifneq ($(ZEDBSD_PLATFORM_DIR),amd64)
$(error WS006 IN-T41 requires an amd64/PC-AT configuration)
endif

WS006_P008_PROBE_SOURCE := \
	plan/ws006-input/tests/usb-hid-guest-probe.c
WS006_P008_PROBE_OBJECT := \
	$(BUILD)/user64/plan/ws006-input/tests/usb-hid-guest-probe.o
WS006_P008_PROBE := $(BUILD)/tests/usb-hid-guest-probe
WS006_P008_UFS := $(ARCH_IMAGE_DIR)/amd64-ws006-p008.ufs
WS006_P008_IMAGE := $(BUILD)/tests/ws006-p008-hdd-image.img

$(WS006_P008_PROBE_OBJECT): $(WS006_P008_PROBE_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(WS006_P008_PROBE): $(AMD64_USER_LIBC_OBJS) \
	$(WS006_P008_PROBE_OBJECT) $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS006_P008_PROBE_OBJECT) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) \
		--machine amd64 $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS006_P008_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS006_P008_PROBE) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) \
	--file /usr/bin/usb-hid-guest-probe=$(WS006_P008_PROBE)))

$(WS006_P008_IMAGE): $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(WS006_P008_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/zedbsd.cfg \
	platform/amd64/tools/check-amd64-gpt-image.noct
	@mkdir -p $(dir $@)
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct \
		--backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force \
		--machine pcat --gpt \
		--checker platform/amd64/tools/check-amd64-gpt-image.noct \
		--checker-runner $(NOCT) \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		--kernel $(BUILD)/vmunix \
		--bootx64 $(BUILD)/uefi/BOOTX64.EFI \
		--zedbsd-config platform/amd64/zedbsd.cfg \
		--arch-profile amd64 --arch-image $(WS006_P008_UFS) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@

.PHONY: qemu-usb-hid-image
qemu-usb-hid-image: $(WS006_P008_IMAGE)
