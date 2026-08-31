# WS004 p019 test-only amd64 CDC ECM guest/image rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ifneq ($(ZEDBSD_PLATFORM_DIR),amd64)
$(error WS004 p019 requires an amd64/PC-AT configuration)
endif

WS004_P019_PROBE_SOURCE := \
	plan/ws004-hardware/tests/usb-cdc-ecm-guest-probe.c
WS004_P019_PROBE_OBJECT := \
	$(BUILD)/user64/plan/ws004-hardware/tests/usb-cdc-ecm-guest-probe.o
WS004_P019_PROBE := $(BUILD)/tests/usb-cdc-ecm-guest-probe
WS004_P019_STATIC_RC := \
	plan/ws004-hardware/tests/qemu-usb-cdc-ecm-rc-static.conf
WS004_P019_DHCP_RC := \
	plan/ws004-hardware/tests/qemu-usb-cdc-ecm-rc-dhcp.conf
WS004_P019_STATIC_SERVICE := \
	plan/ws004-hardware/tests/qemu-usb-cdc-ecm-service-static
WS004_P019_DHCP_SERVICE := \
	plan/ws004-hardware/tests/qemu-usb-cdc-ecm-service-dhcp
WS004_P019_STATIC_UFS := $(ARCH_IMAGE_DIR)/amd64-ws004-p019-static.ufs
WS004_P019_DHCP_UFS := $(ARCH_IMAGE_DIR)/amd64-ws004-p019-dhcp.ufs
WS004_P019_STATIC_IMAGE := $(BUILD)/tests/ws004-p019-static.img
WS004_P019_DHCP_IMAGE := $(BUILD)/tests/ws004-p019-dhcp.img

$(WS004_P019_PROBE_OBJECT): $(WS004_P019_PROBE_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(WS004_P019_PROBE): $(AMD64_USER_LIBC_OBJS) \
	$(WS004_P019_PROBE_OBJECT) $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS004_P019_PROBE_OBJECT) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) \
		--machine amd64 $@

WS004_P019_STATIC_FILES := $(subst \
	--file /etc/rc.conf=userland/base/etc/rc.conf,\
	--file /etc/rc.conf=$(WS004_P019_STATIC_RC),$(AMD64_ARCH_FILES))
WS004_P019_DHCP_FILES := $(subst \
	--file /etc/rc.conf=userland/base/etc/rc.conf,\
	--file /etc/rc.conf=$(WS004_P019_DHCP_RC),$(AMD64_ARCH_FILES))

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS004_P019_STATIC_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS004_P019_PROBE) $(WS004_P019_STATIC_RC) \
	$(WS004_P019_STATIC_SERVICE),\
	$(WS004_P019_STATIC_FILES) \
	--file /usr/bin/usb-cdc-ecm-guest-probe=$(WS004_P019_PROBE) \
	--file /etc/service.d/ecm_qemu=$(WS004_P019_STATIC_SERVICE)))

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS004_P019_DHCP_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS004_P019_PROBE) $(WS004_P019_DHCP_RC) \
	$(WS004_P019_DHCP_SERVICE),\
	$(WS004_P019_DHCP_FILES) \
	--file /usr/bin/usb-cdc-ecm-guest-probe=$(WS004_P019_PROBE) \
	--file /etc/service.d/ecm_qemu=$(WS004_P019_DHCP_SERVICE)))

define WS004_P019_IMAGE_RULE
$(1): $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(2) \
	$(DATA_IMAGE) $(SWAP_IMAGE) $(BUILD)/uefi/BOOTX64.EFI \
	tools/build/make-bios-hdd-image.noct platform/amd64/zedbsd.cfg \
	platform/amd64/tools/check-amd64-gpt-image.noct
	@mkdir -p $$(dir $$@)
	$$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct \
		--backend $$(abspath $$(ZEDBSD_IMAGE_HOST)) --force \
		--machine pcat --gpt \
		--checker platform/amd64/tools/check-amd64-gpt-image.noct \
		--checker-runner $$(NOCT) \
		--stage1 $$(BUILD)/bootloader/stage1.bin \
		--stage2 $$(BUILD)/bootloader/stage2.bin \
		--partition-pbr $$(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $$(BUILD)/bootloader/BOOTZBSD.EXE \
		--kernel $$(BUILD)/vmunix \
		--bootx64 $$(BUILD)/uefi/BOOTX64.EFI \
		--zedbsd-config platform/amd64/zedbsd.cfg \
		--arch-profile amd64 --arch-image $(2) \
		--arch-format ufs --data-image $$(DATA_IMAGE) \
		--swapfile $$(SWAP_IMAGE) $$@
endef

$(eval $(call WS004_P019_IMAGE_RULE,$(WS004_P019_STATIC_IMAGE),\
	$(WS004_P019_STATIC_UFS)))
$(eval $(call WS004_P019_IMAGE_RULE,$(WS004_P019_DHCP_IMAGE),\
	$(WS004_P019_DHCP_UFS)))

.PHONY: ws004-p019-qemu-images
ws004-p019-qemu-images: $(WS004_P019_STATIC_IMAGE) \
	$(WS004_P019_DHCP_IMAGE)
