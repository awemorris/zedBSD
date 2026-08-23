QEMU ?= qemu-system-i386
QEMU_FLAGS ?= -M pc9821,pegc=off,coregraph=on -cpu 486 -smp 1 -m 64M

.PHONY: run
run: disk-image
	@command -v "$(QEMU)" >/dev/null 2>&1 || { \
		echo "PC-98-capable QEMU not found: $(QEMU)" >&2; \
		echo "Set QEMU=/path/to/qemu-system-i386 if it is not in PATH." >&2; \
		exit 2; \
	}
	$(QEMU) $(QEMU_FLAGS) \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$(abspath $(DISK_IMAGE_ARTIFACT))"
