X68K_BOOTLOADER_FILES := $(BUILD)/stage1.bin $(BUILD)/stage2.bin

.PHONY: bootloader
bootloader: $(X68K_BOOTLOADER_FILES)
