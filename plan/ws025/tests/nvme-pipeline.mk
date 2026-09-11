# Private 64 KiB raw-I/O helper; no production command or diagnostic ABI.
include plan/ws019/tests/storage-qemu.mk
WS025_NVME_GUEST := $(BUILD)/tests/nvme-pipeline
WS025_NVME_OBJECT := $(BUILD)/tests/nvme-pipeline.o
$(WS025_NVME_OBJECT): plan/ws004/tests/nvme-io-guest.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -DNVME_GUEST_BYTES=65536U -c $< -o $@
$(WS025_NVME_GUEST): $(AMD64_USER_LIBC_OBJS) $(WS025_NVME_OBJECT) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(WS025_NVME_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
WS025_NVME_BIO := $(BUILD)/tests/nvme-bio
WS025_NVME_BIO_OBJECT := $(BUILD)/tests/nvme-bio.o
$(WS025_NVME_BIO_OBJECT): plan/ws025/tests/nvme-pipeline-guest.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@
$(WS025_NVME_BIO): $(AMD64_USER_LIBC_OBJS) $(WS025_NVME_BIO_OBJECT) $(AMD64_PLATFORM)/user.ld
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(WS025_NVME_BIO_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(ARCH_IMAGE_DIR)/amd64-ws025-nvme.ufs,amd64,\
	$(AMD64_ARCH_INPUTS) $(WS025_NVME_GUEST) $(WS025_NVME_BIO) $(WS019_GUEST) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/nvme-bio=$(WS025_NVME_BIO) --file /usr/bin/nvme-pipeline=$(WS025_NVME_GUEST) \
	--file /usr/bin/storage-exit=$(WS019_GUEST)))
.PHONY: ws025-nvme-fixture
ws025-nvme-fixture: $(ARCH_IMAGE_DIR)/amd64-ws025-nvme.ufs

WS025_NVME_KERNEL := $(BUILD)/tests/nvme-pipeline-kernel.o
AMD64_VMUNIX_OBJS += $(WS025_NVME_KERNEL)
$(WS025_NVME_KERNEL): plan/ws025/tests/nvme-pipeline-kernel.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -c $< -o $@
.PHONY: ws025-nvme-relink
$(BUILD)/vmunix: $(WS025_NVME_KERNEL) ws025-nvme-relink
$(BUILD)/vmunix: LD += --wrap=disk_ioctl
