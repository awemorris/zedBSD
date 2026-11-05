#
# QEMU does not run the Pi's GPU firmware, so it is given the kernel
# and the device tree directly. The SD card carries the same image as
# real hardware.
#

QEMU ?= qemu-system-aarch64
QEMU_FLAGS ?= -machine raspi4b -m 2G -display none -serial mon:stdio -serial null

.PHONY: run

run: disk-image
	$(QEMU) $(QEMU_FLAGS) -kernel $(abspath $(BUILD)/vmunix) \
		-dtb $(abspath $(RPI4_BOOT_FIRMWARE_DTB)) \
		-drive "file=$(abspath $(DISK_IMAGE_ARTIFACT)),format=raw,if=sd,snapshot=on"
