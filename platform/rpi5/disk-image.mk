DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

.PHONY: disk-image check-disk-image
disk-image: world bootloader $(DISK_IMAGE_ARTIFACT)

ifeq ($(ZEDBSD_BOARD),odroid-c5)
check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) tools/build/check-ufs-image.py $(BUILD)/ufs-root.img
else
check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) platform/arm64/tools/check-rpi4-hdd-image.py \
		--kernel $(BUILD)/vmunix --ufs-root $(BUILD)/ufs-root.img \
		--data-image $(DATA_IMAGE) --swapfile $(SWAP_IMAGE) \
		--config $(BUILD)/config.txt --cmdline $(BUILD)/cmdline.txt \
		--board $(ZEDBSD_BOARD) $<
endif
