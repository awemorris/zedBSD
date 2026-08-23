QEMU ?= qemu-system-x86_64
QEMU_FLAGS ?= -machine pc -m 512 -smp 4

.PHONY: run
run: disk-image
	$(QEMU) $(QEMU_FLAGS) \
		-drive "file=$(abspath $(DISK_IMAGE_ARTIFACT)),format=raw,if=ide" -boot c
