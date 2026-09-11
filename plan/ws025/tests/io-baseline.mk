# Disposable WS025 baseline image; never installed in the ordinary root.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ifndef WS019_GUEST
include plan/ws019/tests/storage-qemu.mk
endif
WS025_GUEST := $(BUILD)/tests/io-baseline
WS025_OBJECT := $(BUILD)/tests/io-baseline.o
$(WS025_OBJECT): plan/ws025/tests/io-baseline-guest.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@
$(WS025_GUEST): $(AMD64_USER_LIBC_OBJS) $(WS025_OBJECT) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(WS025_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(ARCH_IMAGE_DIR)/amd64-ws025.ufs,amd64,\
	$(AMD64_ARCH_INPUTS) $(WS025_GUEST) $(WS019_GUEST) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/io-baseline=$(WS025_GUEST) \
	--file /usr/bin/storage-exit=$(WS019_GUEST)))
.PHONY: ws025-baseline-fixture
ws025-baseline-fixture: $(ARCH_IMAGE_DIR)/amd64-ws025.ufs
