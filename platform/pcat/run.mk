QEMU ?= qemu-system-i386
QEMU_FLAGS ?= -machine pc -m 128

.PHONY: run
run: disk-image
	$(QEMU) $(QEMU_FLAGS) \
		-drive "file=$(abspath $(DISK_IMAGE_ARTIFACT)),format=raw,if=ide" -boot c
