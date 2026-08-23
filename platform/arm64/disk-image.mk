DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

.PHONY: disk-image check-disk-image
disk-image: world bootloader $(DISK_IMAGE_ARTIFACT)

check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) platform/arm64/tools/check-rpi4-hdd-image.py \
		--kernel $(BUILD)/vmunix --arch-image $(AARCH64_ARCH_IMAGE) \
		--data-image $(DATA_IMAGE) --swapfile $(SWAP_IMAGE) \
		--config $(ARM64_PLATFORM)/config.txt $<
