# WS001-p015 test-only effective-credential VFS image rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ifneq ($(ZEDBSD_PLATFORM_DIR),amd64)
$(error WS001-p015 QEMU acceptance requires an amd64/PC-AT configuration)
endif

WS001_P015_SOURCE := \
	plan/ws001-posix/tests/credential-vfs-native.c
WS001_P015_OBJECT := \
	$(BUILD)/user64/plan/ws001-posix/tests/credential-vfs-native.o
WS001_P015_PROGRAM := $(BUILD)/tests/credential-vfs-native
WS001_P015_OVERLAY_UFS := \
	$(ARCH_IMAGE_DIR)/amd64-ws001-p015-overlay.ufs
WS001_P015_NATIVE_ARCH_UFS := \
	$(ARCH_IMAGE_DIR)/amd64-ws001-p015-native.ufs
WS001_P015_FAT_UFS := $(ARCH_IMAGE_DIR)/amd64-ws001-p015-fat.ufs
WS001_P015_NATIVE_ROOT := $(BUILD)/ws001-p015-native-root.img
WS001_P015_OVERLAY_IMAGE := $(BUILD)/ws001-p015-overlay-hdd-image.img
WS001_P015_NATIVE_IMAGE := $(BUILD)/ws001-p015-native-hdd-image.img
WS001_P015_FAT_IMAGE := $(BUILD)/ws001-p015-fat-hdd-image.img
WS001_P015_OVERLAY_CONFIG := \
	plan/ws001-posix/tests/credential-vfs-overlay.cfg
WS001_P015_NATIVE_CONFIG := \
	plan/ws001-posix/tests/credential-vfs-native.cfg

$(WS001_P015_OBJECT): $(WS001_P015_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(WS001_P015_PROGRAM): $(AMD64_USER_LIBC_OBJS) $(WS001_P015_OBJECT) \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS001_P015_OBJECT) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) \
		--machine amd64 $@

WS001_P015_COMMON_FILES := \
	$(AMD64_ARCH_FILES) \
	--file /bin/ws001-p015=$(WS001_P015_PROGRAM) \
	--mode /bin/ws001-p015=0755

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS001_P015_OVERLAY_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS001_P015_PROGRAM) \
	plan/ws001-posix/tests/credential-vfs-overlay.txt \
	plan/ws001-posix/tests/credential-vfs-lower-marker.txt \
	$(ZEDBSD_CONFIG),\
	$(WS001_P015_COMMON_FILES) \
	--file /etc/ws001-p015-scenario=plan/ws001-posix/tests/credential-vfs-overlay.txt \
	--file /p015-lower-only/marker=plan/ws001-posix/tests/credential-vfs-lower-marker.txt \
	--mode /p015-lower-only=0777))

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS001_P015_NATIVE_ARCH_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS001_P015_PROGRAM) \
	plan/ws001-posix/tests/credential-vfs-native.txt $(ZEDBSD_CONFIG),\
	$(WS001_P015_COMMON_FILES) \
	--file /etc/ws001-p015-scenario=plan/ws001-posix/tests/credential-vfs-native.txt))

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS001_P015_FAT_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS001_P015_PROGRAM) \
	plan/ws001-posix/tests/credential-vfs-fat.txt $(ZEDBSD_CONFIG),\
	$(WS001_P015_COMMON_FILES) \
	--file /etc/ws001-p015-scenario=plan/ws001-posix/tests/credential-vfs-fat.txt))

$(WS001_P015_NATIVE_ROOT): $(WS001_P015_NATIVE_ARCH_UFS) \
	$(BUILD_TOOLS_DIR)/make-ufs1-root-image.py tools/build/ufs1_format.py
	$(PYTHON) $(BUILD_TOOLS_DIR)/make-ufs1-root-image.py --force \
		--arch-profile amd64 --arch-image $(WS001_P015_NATIVE_ARCH_UFS) $@

define WS001_P015_OVERLAY_IMAGE_RULE
$(1): $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2-chain.bin \
	$(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(2) \
	$(DATA_IMAGE) $(BUILD)/uefi/BOOTX64.EFI $(WS001_P015_OVERLAY_CONFIG) \
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
		--zedbsd-config $(WS001_P015_OVERLAY_CONFIG) \
		--arch-profile amd64 --arch-image $(2) --arch-format ufs \
		--data-image $(DATA_IMAGE) $$@
endef

$(eval $(call WS001_P015_OVERLAY_IMAGE_RULE,$(WS001_P015_OVERLAY_IMAGE),\
	$(WS001_P015_OVERLAY_UFS)))
$(eval $(call WS001_P015_OVERLAY_IMAGE_RULE,$(WS001_P015_FAT_IMAGE),\
	$(WS001_P015_FAT_UFS)))

$(WS001_P015_NATIVE_IMAGE): $(BUILD)/bootloader/stage1-native.bin \
	$(BUILD)/bootloader/stage2-chain.bin \
	$(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(WS001_P015_NATIVE_ROOT) $(WS001_P015_NATIVE_CONFIG) \
	$(ZEDBSD_IMAGE_HOST) $(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct \
	$(BUILD_TOOLS_DIR)/check-bios-hdd-image.noct
	$(NOCT) --path=$(BUILD_TOOLS_DIR) \
		$(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct \
		--backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force \
		--checker $(BUILD_TOOLS_DIR)/check-bios-hdd-image.noct \
		--checker-runner $(NOCT) --machine pcat \
		--stage1 $(BUILD)/bootloader/stage1-native.bin \
		--stage2 $(BUILD)/bootloader/stage2-chain.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		--kernel $(BUILD)/vmunix \
		--zedbsd-config $(WS001_P015_NATIVE_CONFIG) \
		--ufs-root $(WS001_P015_NATIVE_ROOT) --size-mib 193 $@

.PHONY: ws001-p015-qemu-images ws001-p015-guest-probe
ws001-p015-guest-probe: $(WS001_P015_PROGRAM)
ws001-p015-qemu-images: $(WS001_P015_OVERLAY_IMAGE) \
	$(WS001_P015_NATIVE_IMAGE) $(WS001_P015_FAT_IMAGE)
