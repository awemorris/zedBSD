# Test-only executable and image; ordinary dhcpc and images remain unchanged.
P033_DHCP := $(BUILD)/tests/p033-dhcpc
P033_DHCP_OBJECT := $(BUILD)/user64/plan/ws025/tests/p033-dhcpc-drop.o
P033_DHCP_UFS := $(ARCH_IMAGE_DIR)/amd64-p033-dhcp.ufs
P033_DHCP_IMAGE := $(BUILD)/p033-dhcp.img

$(P033_DHCP_OBJECT): userland/base/dhcpc/main.c
$(P033_DHCP): $(AMD64_USER_NET_LIBC_OBJS) $(AMD64_USER_NET_COMMON_OBJS) \
	$(P033_DHCP_OBJECT) $(AMD64_PLATFORM)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(AMD64_USER_NET_COMMON_OBJS) $(P033_DHCP_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(P033_DHCP_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(P033_DHCP) $(ZEDBSD_CONFIG),\
	$(AMD64_ARCH_FILES) --file /usr/bin/p033-dhcpc=$(P033_DHCP)))

$(P033_DHCP_IMAGE): $(BUILD)/hdd-image.img $(P033_DHCP_UFS)
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct \
	 --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat \
	 --layout $(ZEDBSD_VARIANT) \
	 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
	 --checker-runner $(NOCT) --stage1 $(AMD64_IMAGE_STAGE1) \
	 --stage2 $(BUILD)/bootloader/stage2-chain.bin \
	 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
	 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
	 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
	 --zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
	 --arch-profile amd64 --arch-image $(P033_DHCP_UFS) \
	 --arch-format ufs --data-image $(DATA_IMAGE) --swapfile $(SWAP_IMAGE) $@

.PHONY: p033-dhcp-fixture
p033-dhcp-fixture: $(P033_DHCP_IMAGE)
