DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

.PHONY: disk-image check-disk-image
disk-image: world bootloader $(DISK_IMAGE_ARTIFACT)

check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) $(BUILD_TOOLS_DIR)/check-bios-hdd-image.py --machine pc98 \
		--kernel $(BUILD)/vmunix --arch-profile i386 \
		--arch-image $(I386_ARCH_UFS_IMAGE) --arch-format ufs $<
