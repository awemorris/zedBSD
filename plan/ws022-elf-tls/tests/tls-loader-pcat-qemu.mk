TLS_FIXTURES ?= plan/ws022-elf-tls/temp/q128-fixtures
# WS022 TLS test-only i386 probe/image rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib



WS022_P002_PROBE_SOURCE := \
	plan/ws022-elf-tls/tests/tls-loader-probe.c
WS022_P002_PROBE_OBJECT := \
	$(BUILD)/user64/plan/ws022-elf-tls/tests/tls-loader-probe.o
WS022_P002_PROBE := $(BUILD)/tests/tls-loader-probe
WS022_P002_UFS := $(ARCH_IMAGE_DIR)/i386-ws022-p002.ufs
WS022_P002_IMAGE := $(BUILD)/ws022-p002-hdd-image.img

$(WS022_P002_PROBE_OBJECT): $(WS022_P002_PROBE_SOURCE) $(ZEDBSD_SYSROOT_I386)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_CPPFLAGS) $(USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(WS022_P002_PROBE): $(USER_LIBC_OBJS) $(ZEDBSD_SOFTFLOAT_OBJECTS) \
	$(WS022_P002_PROBE_OBJECT) platform/pcat/user.ld \
	$(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T platform/pcat/user.ld $(USER_LIBC_OBJS) \
		$(WS022_P002_PROBE_OBJECT) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(USER_ELF_CHECK) \
		--machine i386 $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS022_P002_UFS),i386,\
	$(I386_ARCH_INPUTS) $(WS022_P002_PROBE) $(wildcard $(TLS_FIXTURES)/i386*.elf) $(ZEDBSD_CONFIG),\
	$(I386_ARCH_FILES) \
	--file /usr/bin/tls-loader-probe=$(WS022_P002_PROBE) \
	$(foreach f,$(wildcard $(TLS_FIXTURES)/i386*.elf),--file /usr/share/tls/$(notdir $(f))=$(f))))

$(WS022_P002_IMAGE): $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2-chain.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(WS022_P002_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) $(PCAT_ZEDBSD_CONFIG)
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct \
		--backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force \
		--machine pcat --checker tools/build/check-bios-hdd-image.noct \
		--checker-runner $(NOCT) \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2-chain.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE --kernel $(BUILD)/vmunix \
		--zedbsd-config $(PCAT_ZEDBSD_CONFIG) \
		--arch-profile i386 --arch-image $(WS022_P002_UFS) \
		--arch-format ufs --data-image $(DATA_IMAGE) --swapfile $(SWAP_IMAGE) \
		--size-mib 177 --fat-size-mib 176 $@

.PHONY: ws022-p002-qemu-image
ws022-p002-qemu-image: $(WS022_P002_IMAGE)
