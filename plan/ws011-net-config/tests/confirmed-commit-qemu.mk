# WS011 p007 test-only confirmed-commit image rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ifneq ($(ZEDBSD_PLATFORM_DIR),amd64)
$(error WS011 p007 QEMU acceptance requires amd64/PC-AT)
endif

WS011_P007_NETCONF := \
	plan/ws011-net-config/tests/confirmed-commit-qemu-net.conf
WS011_P007_UFS := $(ARCH_IMAGE_DIR)/amd64-ws011-p007.ufs
WS011_P007_IMAGE := $(BUILD)/tests/ws011-p007-confirmed.img

# The phase-owned synthetic net.conf deliberately follows the normal manifest
# so it replaces the shipping loopback-only file only in this private image.
WS011_P007_FILES := \
	$(AMD64_ARCH_FILES) \
	--file /etc/net.conf=$(WS011_P007_NETCONF) \
	--mode /etc/net.conf=0644

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS011_P007_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS011_P007_NETCONF) $(ZEDBSD_CONFIG),\
	$(WS011_P007_FILES)))

$(WS011_P007_IMAGE): $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2-chain.bin \
	$(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(WS011_P007_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI platform/amd64/zedbsd.cfg \
	$(ZEDBSD_CONFIG) \
	plan/ws011-net-config/tests/confirmed-commit-qemu.mk \
	$(ZEDBSD_IMAGE_HOST) $(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=$(BUILD_TOOLS_DIR) \
		$(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct \
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
		--arch-profile amd64 --arch-image $(WS011_P007_UFS) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@

.PHONY: ws011-p007-qemu-image
ws011-p007-qemu-image: $(WS011_P007_IMAGE)
