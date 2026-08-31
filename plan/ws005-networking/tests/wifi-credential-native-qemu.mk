# WS005 p005 test-only native credential image rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ifneq ($(ZEDBSD_PLATFORM_DIR),amd64)
$(error WS005 p005 native QEMU acceptance requires amd64/PC-AT)
endif

WS005_P005_SOURCE := \
	plan/ws005-networking/tests/wifi-credential-native.c
WS005_P005_OBJECT := \
	$(BUILD)/user64/plan/ws005-networking/tests/wifi-credential-native.o
WS005_P005_PROGRAM := $(BUILD)/tests/wifi-credential-native
WS005_P005_UFS := $(ARCH_IMAGE_DIR)/amd64-ws005-p005.ufs
WS005_P005_IMAGE := $(BUILD)/tests/ws005-p005-overlay.img
WS005_P005_CONFIG := \
	plan/ws005-networking/tests/wifi-credential-native.cfg
WS005_P005_PASSWD := \
	plan/ws005-networking/tests/wifi-credential-native-passwd
WS005_P005_GROUP := \
	plan/ws005-networking/tests/wifi-credential-native-group
WS005_P005_NETCONF := userland/base/etc/net.conf

$(WS005_P005_OBJECT): $(WS005_P005_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(WS005_P005_PROGRAM): $(AMD64_USER_LIBC_OBJS) $(WS005_P005_OBJECT) \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS005_P005_OBJECT) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(AMD64_USER_ELF_CHECK) \
		--machine amd64 $@

# The test passwd/group files deliberately follow the normal architecture
# manifest so they override the root-only shipping account files in this
# private image and nowhere else.
WS005_P005_FILES := \
	$(AMD64_ARCH_FILES) \
	--file /bin/ws005-p005=$(WS005_P005_PROGRAM) \
	--mode /bin/ws005-p005=0755 \
	--file /etc/passwd=$(WS005_P005_PASSWD) \
	--mode /etc/passwd=0644 \
	--file /etc/group=$(WS005_P005_GROUP) \
	--mode /etc/group=0644 \
	--file /etc/net.conf=$(WS005_P005_NETCONF) \
	--mode /etc/net.conf=0644

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS005_P005_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS005_P005_PROGRAM) \
	$(WS005_P005_PASSWD) $(WS005_P005_GROUP) $(WS005_P005_NETCONF) \
	$(ZEDBSD_CONFIG),\
	$(WS005_P005_FILES)))

$(WS005_P005_IMAGE): $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2-chain.bin \
	$(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(WS005_P005_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI $(WS005_P005_CONFIG) \
	$(ZEDBSD_IMAGE_HOST) $(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=$(BUILD_TOOLS_DIR) \
		$(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct \
		--backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force \
		--machine pcat --gpt \
		--checker platform/amd64/tools/check-amd64-gpt-image.noct \
		--checker-runner $(NOCT) \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2-chain.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		--kernel $(BUILD)/vmunix \
		--bootx64 $(BUILD)/uefi/BOOTX64.EFI \
		--zedbsd-config $(WS005_P005_CONFIG) \
		--arch-profile amd64 --arch-image $(WS005_P005_UFS) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@

.PHONY: ws005-p005-native-qemu-image ws005-p005-native-guest-probe
ws005-p005-native-qemu-image: $(WS005_P005_IMAGE)
ws005-p005-native-guest-probe: $(WS005_P005_PROGRAM)
