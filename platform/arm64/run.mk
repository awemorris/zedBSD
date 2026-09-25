QEMU ?= qemu-system-aarch64
# QEMU does not run the Pi's GPU firmware, so it is given the kernel and the
# device tree directly; the SD card carries the same image as real hardware.
QEMU_FLAGS ?= -machine raspi4b -m 2G -display none -serial mon:stdio \
	-serial null

.PHONY: run
run: disk-image
	$(QEMU) $(QEMU_FLAGS) -kernel $(abspath $(BUILD)/vmunix) \
		-dtb $(abspath vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb) \
		-drive "file=$(abspath $(DISK_IMAGE_ARTIFACT)),format=raw,if=sd,snapshot=on"
