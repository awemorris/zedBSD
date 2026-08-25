DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

.PHONY: disk-image check-disk-image
disk-image: world bootloader $(DISK_IMAGE_ARTIFACT)

check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(NOCT) --path=tools/build platform/amd64/tools/check-amd64-gpt-image.noct --machine pcat \
		--kernel $(BUILD)/vmunix \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE --arch-profile amd64 \
		--bootx64 $(BUILD)/uefi/BOOTX64.EFI \
		--arch-image $(AMD64_ARCH_UFS_IMAGE) --arch-format ufs \
		--data-image $(DATA_IMAGE) --swapfile $(SWAP_IMAGE) $<
