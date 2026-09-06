# Test-only formatter observer and image; uses the existing storage observer.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws019-installation/tests/storage-qemu.mk

WS019_FORMAT_PROBE := $(BUILD)/tests/formatter-probe
WS019_FORMAT_OBJECT := $(BUILD)/user64/plan/ws019-installation/tests/formatter-probe-guest.o
WS019_FORMAT_UFS := $(ARCH_IMAGE_DIR)/amd64-ws019-formatters.ufs

$(WS019_FORMAT_PROBE): $(AMD64_USER_LIBC_OBJS) $(WS019_FORMAT_OBJECT) $(AMD64_PLATFORM)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS019_FORMAT_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS019_FORMAT_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS019_GUEST) $(WS019_FORMAT_PROBE) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/storage-exit=$(WS019_GUEST) \
	--file /usr/bin/formatter-probe=$(WS019_FORMAT_PROBE)))

.PHONY: ws019-formatter-qemu-fixture
ws019-formatter-qemu-fixture: $(WS019_FORMAT_UFS)
