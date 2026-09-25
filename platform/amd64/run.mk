QEMU ?= qemu-system-x86_64
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd

ifeq ($(ZEDBSD_VARIANT),native)
# The native image boots only through UEFI: OVMF, with a copy of its
# variables that the run may change, and the disk on NVMe.
QEMU_FLAGS ?= -machine q35 -m 8192 -smp 4

.PHONY: run
run: disk-image
	cp $(OVMF_VARS) $(BUILD)/run-ovmf-vars.fd
	$(QEMU) $(QEMU_FLAGS) \
		-drive "if=pflash,format=raw,readonly=on,file=$(OVMF_CODE)" \
		-drive "if=pflash,format=raw,file=$(abspath $(BUILD))/run-ovmf-vars.fd" \
		-drive "file=$(abspath $(DISK_IMAGE_ARTIFACT)),format=raw,if=none,id=boot" \
		-device nvme,drive=boot,serial=zedbsd
else
QEMU_FLAGS ?= -machine pc -m 512 -smp 4

.PHONY: run
run: disk-image
	$(QEMU) $(QEMU_FLAGS) \
		-drive "file=$(abspath $(DISK_IMAGE_ARTIFACT)),format=raw,if=ide" -boot c
endif
