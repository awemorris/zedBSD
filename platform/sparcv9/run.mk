QEMU ?= qemu-system-sparc64
QEMU_FLAGS ?= -machine sun4u -m 256

.PHONY: run
run: disk-image
	$(QEMU) $(QEMU_FLAGS) \
		-drive "file=$(abspath $(DISK_IMAGE_ARTIFACT)),format=raw,if=ide" -boot c
