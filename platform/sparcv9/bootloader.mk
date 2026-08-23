SPARCV9_BOOTLOADER_FILES := $(BUILD)/boot/stage1.bin $(BUILD)/boot/stage2.bin

.PHONY: bootloader
bootloader: $(SPARCV9_BOOTLOADER_FILES)
	$(PYTHON) platform/sparcv9/tools/check-sparcv9-boot.py \
		--stage1 $(BUILD)/boot/stage1.bin --stage2 $(BUILD)/boot/stage2.bin
