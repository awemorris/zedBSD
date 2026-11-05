RPI4_FIRMWARE_FILES := \
	$(RPI4_BOOT_FIRMWARE_CACHE_FILES) \
	$(BUILD)/bcm2711-rpi-4-b.dtb \
	$(BUILD)/config.txt

.PHONY: bootloader
bootloader: $(RPI4_FIRMWARE_FILES)

$(BUILD)/bcm2711-rpi-4-b.dtb: $(RPI4_BOOT_FIRMWARE_DTB)
	@mkdir -p $(dir $@)
	cp $< $@
