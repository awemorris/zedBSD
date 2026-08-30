AMD64_BOOTLOADER_FILES := $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2-chain.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/uefi/BOOTX64.EFI

.PHONY: bootloader
bootloader: $(AMD64_BOOTLOADER_FILES)
