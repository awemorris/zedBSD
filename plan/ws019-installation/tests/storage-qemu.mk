# Test-only wrapper/image; never installed in the ordinary production image.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ifneq ($(ZEDBSD_PLATFORM_DIR),amd64)
$(error WS019 storage QEMU requires amd64/PC-AT)
endif
WS019_GUEST_OBJECT := $(BUILD)/tests/storage-exit.o
WS019_GUEST := $(BUILD)/tests/storage-exit
WS019_UFS := $(ARCH_IMAGE_DIR)/amd64-ws019-storage.ufs

$(WS019_GUEST_OBJECT): plan/ws019-installation/tests/storage-exit-guest.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@

$(WS019_GUEST): $(AMD64_USER_LIBC_OBJS) $(WS019_GUEST_OBJECT) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(WS019_GUEST_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS019_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(WS019_GUEST) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/storage-exit=$(WS019_GUEST)))

.PHONY: ws019-storage-qemu-fixture
ws019-storage-qemu-fixture: $(WS019_UFS)
