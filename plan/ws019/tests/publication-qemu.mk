# Test-only formatter observer and image; uses the existing storage observer.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws019/tests/storage-qemu.mk

WS019_PUBLICATION_PROBE := $(BUILD)/tests/publication-probe
WS019_PUBLICATION_OBJECT := $(BUILD)/user64/plan/ws019/tests/publication-probe-guest.o
WS019_PUBLICATION_UFS := $(ARCH_IMAGE_DIR)/amd64-ws019-publication.ufs

$(WS019_PUBLICATION_PROBE): $(AMD64_USER_LIBC_OBJS) $(WS019_PUBLICATION_OBJECT) $(AMD64_PLATFORM)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS019_PUBLICATION_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS019_PUBLICATION_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS019_GUEST) $(WS019_PUBLICATION_PROBE) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/storage-exit=$(WS019_GUEST) \
	--file /usr/bin/publication-probe=$(WS019_PUBLICATION_PROBE)))

.PHONY: ws019-publication-qemu-fixture
ws019-publication-qemu-fixture: $(WS019_PUBLICATION_UFS)
