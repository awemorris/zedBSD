RPI4_FIRMWARE_FILES := vendor/raspberrypi-firmware/boot/start4.elf \
	vendor/raspberrypi-firmware/boot/fixup4.dat \
	vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb \
	platform/arm64/config.txt

.PHONY: bootloader
bootloader: $(RPI4_FIRMWARE_FILES)
