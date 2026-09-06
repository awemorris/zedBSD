include plan/ws019-installation/tests/storage-qemu.mk
Q086_GUEST := $(BUILD)/tests/storage-native
Q086_OBJECTS := $(BUILD)/tests/storage-native.o $(BUILD)/tests/storage-wifi-store.o $(BUILD)/tests/storage-wifi-conf.o
$(BUILD)/tests/storage-native.o: plan/ws018-kernel-architecture/tests/storage-native-guest.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -DWIFI_STORE_TESTING -c $< -o $@
$(BUILD)/tests/storage-wifi-store.o: userland/base/net/wifi-store.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -DWIFI_STORE_TESTING -c $< -o $@
$(BUILD)/tests/storage-wifi-conf.o: userland/base/net/wifi-conf.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@
$(Q086_GUEST): $(AMD64_USER_LIBC_OBJS) $(Q086_OBJECTS) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(Q086_OBJECTS) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(ARCH_IMAGE_DIR)/amd64-q086.ufs,amd64,\
	$(AMD64_ARCH_INPUTS) $(WS019_GUEST) $(Q086_GUEST) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/storage-exit=$(WS019_GUEST) \
	--file /usr/bin/storage-native=$(Q086_GUEST)))
.PHONY: q086-native-fixture
q086-native-fixture: $(ARCH_IMAGE_DIR)/amd64-q086.ufs
