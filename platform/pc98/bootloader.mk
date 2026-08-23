PC98_BOOTLOADER_FILES := $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE

.PHONY: bootloader
bootloader: $(PC98_BOOTLOADER_FILES)
