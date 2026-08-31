# WS005 p003 test-only PC-98 peer-credential probe/image rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ifneq ($(ZEDBSD_PLATFORM_DIR),pc98)
$(error WS005 peercred native QEMU test requires the PC-98 configuration)
endif

WS005_PEERCRED_SOURCE := \
	plan/ws005-networking/tests/peercred-native-test.c
WS005_PEERCRED_OBJECT := \
	$(BUILD)/user32/plan/ws005-networking/tests/peercred-native-test.o
WS005_PEERCRED_PROGRAM := $(BUILD)/tests/peercred-native-test
WS005_PEERCRED_UFS := $(ARCH_IMAGE_DIR)/i386-ws005-peercred.ufs
WS005_PEERCRED_IMAGE := $(BUILD)/peercred-native-hdd-image.img

$(WS005_PEERCRED_OBJECT): $(WS005_PEERCRED_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_CPPFLAGS) $(USER_CFLAGS) -fno-strict-aliasing \
		-MMD -MP -c $< -o $@

$(WS005_PEERCRED_PROGRAM): $(USER_LIBC_OBJS) $(WS005_PEERCRED_OBJECT) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PC98)/noct-user.ld $(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static \
		-z max-page-size=4096 $(USER_STACK_LDFLAGS) \
		-T $(PC98)/noct-user.ld $(USER_LIBC_OBJS) \
		$(WS005_PEERCRED_OBJECT) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS005_PEERCRED_UFS),i386,\
	$(I386_ARCH_INPUTS) $(WS005_PEERCRED_PROGRAM),\
	$(I386_ARCH_FILES) \
	--file /bin/peercred=$(WS005_PEERCRED_PROGRAM)))

$(WS005_PEERCRED_IMAGE): $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(WS005_PEERCRED_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(PC98_ZEDBSD_CONFIG) $(HOLORIS_NOCT) $(ZEDBSD_IMAGE_HOST) \
	$(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct \
	$(BUILD_TOOLS_DIR)/check-bios-hdd-image.noct
	$(NOCT) --path=$(BUILD_TOOLS_DIR) \
		$(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct \
		--backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force \
		--checker $(BUILD_TOOLS_DIR)/check-bios-hdd-image.noct \
		--checker-runner $(NOCT) --machine pc98 \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		--kernel $(BUILD)/vmunix --zedbsd-config $(PC98_ZEDBSD_CONFIG) \
		--arch-profile i386 --arch-image $(WS005_PEERCRED_UFS) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@

.PHONY: ws005-peercred-native-qemu-image
ws005-peercred-native-qemu-image: $(WS005_PEERCRED_IMAGE)
