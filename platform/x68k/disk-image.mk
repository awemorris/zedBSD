DISK_IMAGE_ARTIFACT := $(BUILD)/zedbsd-x68k.hd

.PHONY: disk-image check-disk-image
disk-image: world bootloader $(DISK_IMAGE_ARTIFACT)

check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) platform/x68k/tools/check-x68k-image.py \
		--stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh $<
	$(PYTHON) tests/x68k-image-host-test.py \
		--checker platform/x68k/tools/check-x68k-image.py \
		--stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh $<
