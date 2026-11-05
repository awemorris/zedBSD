QEMU ?= qemu-system-aarch64
# QEMU does not run the Pi's GPU firmware, so it is given the kernel and the
# device tree directly; the SD card carries the same image as real hardware.
QEMU_FLAGS ?= -machine raspi4b -m 2G -display none -serial mon:stdio \
	-serial null

.PHONY: run
ifeq ($(ZEDBSD_BOARD),odroid-c5)
# QEMU has no Amlogic machine: the image boots on the board alone.
run:
	@echo "run: QEMU has no ODROID-C5 machine; write $(DISK_IMAGE_ARTIFACT) to an SD card" >&2
	@false
else ifeq ($(ZEDBSD_BOARD),rpi5)
# QEMU has no Raspberry Pi 5 machine: the image boots on the board alone.
run:
	@echo "run: QEMU has no Raspberry Pi 5 machine; write $(DISK_IMAGE_ARTIFACT) to an SD card" >&2
	@false
else
run: disk-image
	$(QEMU) $(QEMU_FLAGS) -kernel $(abspath $(BUILD)/vmunix) \
		-dtb $(abspath $(RPI4_BOOT_FIRMWARE_DTB)) \
		-drive "file=$(abspath $(DISK_IMAGE_ARTIFACT)),format=raw,if=sd,snapshot=on"
endif
