# WS004 p023 test-only amd64 raw-NVMe helper/image rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ifneq ($(ZEDBSD_PLATFORM_DIR),amd64)
$(error WS004 p023 requires an amd64/PC-AT configuration)
endif

WS004_P023_GUEST_SOURCE := \
	plan/ws004-hardware/tests/nvme-io-guest.c
WS004_P023_GUEST_OBJECT := \
	$(BUILD)/user64/plan/ws004-hardware/tests/nvme-io-guest.o
WS004_P023_GUEST := $(BUILD)/tests/nvme-io-guest
WS004_P023_UFS := $(ARCH_IMAGE_DIR)/amd64-ws004-p023.ufs
WS004_P023_IMAGE := $(BUILD)/ws004-p023-hdd-image.img

$(WS004_P023_GUEST_OBJECT): $(WS004_P023_GUEST_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(WS004_P023_GUEST): $(AMD64_USER_LIBC_OBJS) \
	$(WS004_P023_GUEST_OBJECT) $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS004_P023_GUEST_OBJECT) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) \
		--machine amd64 $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS004_P023_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS004_P023_GUEST) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) \
	--file /usr/bin/nvme-io-guest=$(WS004_P023_GUEST)))

$(WS004_P023_IMAGE): $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(WS004_P023_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/zedbsd.cfg \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct \
		--backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force \
		--machine pcat --gpt \
		--checker platform/amd64/tools/check-amd64-gpt-image.noct \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		--kernel $(BUILD)/vmunix \
		--bootx64 $(BUILD)/uefi/BOOTX64.EFI \
		--zedbsd-config platform/amd64/zedbsd.cfg \
		--arch-profile amd64 --arch-image $(WS004_P023_UFS) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@

.PHONY: ws004-p023-qemu-image
ws004-p023-qemu-image: $(WS004_P023_IMAGE)
