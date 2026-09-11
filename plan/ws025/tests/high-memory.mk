# Link-only observation; no production policy switch or kernel test syscall.
include plan/ws025/tests/io-baseline.mk
WS025_HIGH_KERNEL := $(BUILD)/tests/high-memory-kernel.o
WS025_HIGH_OBJECT := $(BUILD)/tests/high-memory-guest.o
WS025_HIGH_GUEST := $(BUILD)/tests/high-memory-guest
AMD64_VMUNIX_OBJS += $(WS025_HIGH_KERNEL)
$(WS025_HIGH_KERNEL): plan/ws025/tests/high-memory-kernel.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -c $< -o $@
.PHONY: ws025-high-relink
$(BUILD)/vmunix: $(WS025_HIGH_KERNEL) ws025-high-relink
$(BUILD)/vmunix: LD += --wrap=amd64_boot_memory_release --wrap=hal_space_map --wrap=hal_space_unmap --wrap=hal_pmem_free
$(WS025_HIGH_OBJECT): plan/ws025/tests/high-memory-guest.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@
$(WS025_HIGH_GUEST): $(AMD64_USER_LIBC_OBJS) $(WS025_HIGH_OBJECT) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(WS025_HIGH_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(ARCH_IMAGE_DIR)/amd64-ws025-high.ufs,amd64,\
	$(AMD64_ARCH_INPUTS) $(WS025_HIGH_GUEST) $(WS019_GUEST) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/high-memory=$(WS025_HIGH_GUEST) \
	--file /usr/bin/storage-exit=$(WS019_GUEST)))
.PHONY: ws025-high-fixture
ws025-high-fixture: $(ARCH_IMAGE_DIR)/amd64-ws025-high.ufs $(BUILD)/hdd-image.img
