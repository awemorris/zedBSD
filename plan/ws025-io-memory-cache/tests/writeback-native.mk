include plan/ws019-installation/tests/storage-qemu.mk
WS025_WRITEBACK_GUEST := $(BUILD)/tests/writeback-native
WS025_WRITEBACK_OBJECT := $(BUILD)/tests/writeback-native.o
$(WS025_WRITEBACK_OBJECT): plan/ws025-io-memory-cache/tests/writeback-native.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@
$(WS025_WRITEBACK_GUEST): $(AMD64_USER_LIBC_OBJS) $(WS025_WRITEBACK_OBJECT) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(WS025_WRITEBACK_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
WS025_READ_GUEST := $(BUILD)/tests/readahead-native
WS025_READ_OBJECT := $(BUILD)/tests/readahead-native.o
$(WS025_READ_OBJECT): plan/ws025-io-memory-cache/tests/readahead-native.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@
$(WS025_READ_GUEST): $(AMD64_USER_LIBC_OBJS) $(WS025_READ_OBJECT) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(WS025_READ_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(ARCH_IMAGE_DIR)/amd64-ws025-writeback.ufs,amd64,\
	$(AMD64_ARCH_INPUTS) $(WS025_READ_GUEST) $(WS025_WRITEBACK_GUEST) $(WS019_GUEST) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/readahead-native=$(WS025_READ_GUEST) --file /usr/bin/writeback-native=$(WS025_WRITEBACK_GUEST) \
	--file /usr/bin/storage-exit=$(WS019_GUEST)))
.PHONY: ws025-writeback-fixture
ws025-writeback-fixture: $(ARCH_IMAGE_DIR)/amd64-ws025-writeback.ufs
