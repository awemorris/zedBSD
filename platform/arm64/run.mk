QEMU ?= qemu-system-aarch64
QEMU_FLAGS ?= -machine raspi4b -m 2G

.PHONY: run
run: disk-image
	$(QEMU) $(QEMU_FLAGS) \
		-drive "file=$(abspath $(DISK_IMAGE_ARTIFACT)),format=raw,if=sd"
