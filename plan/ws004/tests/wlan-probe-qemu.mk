# Private exact-device WLAN observer; no credentials enter this build.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

WS004_WLAN_PROBE := $(BUILD)/tests/wlan-probe
WS004_WLAN_OBJECT := $(BUILD)/user64/plan/ws004/tests/wlan-probe-guest.o
WS004_WLAN_STORE := $(BUILD)/user64/userland/base/net/wifi-store.o
WS004_WLAN_CONF := $(BUILD)/user64/userland/base/net/wifi-conf.o
WS004_WLAN_UFS := $(ARCH_IMAGE_DIR)/amd64-ws004-wlan.ufs

$(WS004_WLAN_PROBE): $(AMD64_USER_LIBC_OBJS) $(WS004_WLAN_OBJECT) \
	$(WS004_WLAN_STORE) $(WS004_WLAN_CONF) $(AMD64_PLATFORM)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS004_WLAN_OBJECT) $(WS004_WLAN_STORE) $(WS004_WLAN_CONF) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS004_WLAN_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS004_WLAN_PROBE) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/wlan-probe=$(WS004_WLAN_PROBE)))

.PHONY: ws004-wlan-qemu-fixture
ws004-wlan-qemu-fixture: $(WS004_WLAN_UFS)
