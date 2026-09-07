include plan/ws019-installation/tests/storage-qemu.mk
WS025_CACHE_GUEST := $(BUILD)/tests/file-cache
WS025_CACHE_OBJECT := $(BUILD)/tests/file-cache.o
$(WS025_CACHE_OBJECT): plan/ws025-io-memory-cache/tests/file-cache-native.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@
$(WS025_CACHE_GUEST): $(AMD64_USER_LIBC_OBJS) $(WS025_CACHE_OBJECT) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(WS025_CACHE_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(ARCH_IMAGE_DIR)/amd64-ws025-cache.ufs,amd64,\
	$(AMD64_ARCH_INPUTS) $(WS025_CACHE_GUEST) $(WS019_GUEST) $(ZEDBSD_CONFIG) plan/ws025-io-memory-cache/tests/file-cache-lower.txt,\
	$(AMD64_ARCH_FILES) --file /usr/bin/file-cache=$(WS025_CACHE_GUEST) \
	--file /usr/bin/storage-exit=$(WS019_GUEST) \
	--file /etc/ws025-cache-lower=plan/ws025-io-memory-cache/tests/file-cache-lower.txt))
.PHONY: ws025-cache-fixture
ws025-cache-fixture: $(ARCH_IMAGE_DIR)/amd64-ws025-cache.ufs
