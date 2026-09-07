# Disposable WS024 baseline image; never installed in the ordinary root.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ifndef WS019_GUEST
include plan/ws019-installation/tests/storage-qemu.mk
endif
WS024_GUEST := $(BUILD)/tests/ufs-features
WS024_OBJECT := $(BUILD)/tests/ufs-features.o
$(WS024_OBJECT): plan/ws024-unified-ufs/tests/ufs-features-guest.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@
$(WS024_GUEST): $(AMD64_USER_LIBC_OBJS) $(WS024_OBJECT) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(WS024_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(ARCH_IMAGE_DIR)/amd64-ws024-features.ufs,amd64,\
	$(AMD64_ARCH_INPUTS) $(WS024_GUEST) $(WS019_GUEST) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/ufs-features=$(WS024_GUEST) \
	--file /usr/bin/storage-exit=$(WS019_GUEST)))
.PHONY: ws024-features-fixture
ws024-features-fixture: $(ARCH_IMAGE_DIR)/amd64-ws024-features.ufs
