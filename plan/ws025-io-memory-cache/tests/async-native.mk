include plan/ws019-installation/tests/storage-qemu.mk
WS025_ASYNC_GUEST := $(BUILD)/tests/async-native
WS025_ASYNC_OBJECT := $(BUILD)/tests/async-native.o
$(WS025_ASYNC_OBJECT): plan/ws025-io-memory-cache/tests/async-native.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@
$(WS025_ASYNC_GUEST): $(AMD64_USER_LIBC_OBJS) $(WS025_ASYNC_OBJECT) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(WS025_ASYNC_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(ARCH_IMAGE_DIR)/amd64-ws025-async.ufs,amd64,\
	$(AMD64_ARCH_INPUTS) $(WS025_ASYNC_GUEST) $(WS019_GUEST) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/async-native=$(WS025_ASYNC_GUEST) \
	--file /usr/bin/storage-exit=$(WS019_GUEST)))
.PHONY: ws025-async-fixture
ws025-async-fixture: $(ARCH_IMAGE_DIR)/amd64-ws025-async.ufs

WS025_ASYNC_KERNEL := $(BUILD)/tests/async-native-kernel.o
AMD64_VMUNIX_OBJS += $(WS025_ASYNC_KERNEL)
$(WS025_ASYNC_KERNEL): plan/ws025-io-memory-cache/tests/async-native-kernel.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -c $< -o $@
.PHONY: ws025-async-relink
$(BUILD)/vmunix: $(WS025_ASYNC_KERNEL) ws025-async-relink
$(BUILD)/vmunix: LD += --wrap=disk_ioctl
