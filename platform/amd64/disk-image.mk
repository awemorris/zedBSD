DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

.PHONY: disk-image check-disk-image
disk-image: world bootloader $(DISK_IMAGE_ARTIFACT)

check-disk-image: $(DISK_IMAGE_ARTIFACT)
ifeq ($(ZEDBSD_VARIANT),native)
	$(PYTHON) platform/amd64/tools/check-amd64-native-image.py \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(AMD64_NATIVE_UEFI_ZEDBSD_CONFIG) \
 --ufs-root $(AMD64_NATIVE_ROOT_IMAGE) --swap $(AMD64_NATIVE_SWAP_IMAGE) $<
else
	$(call AMD64_VALIDATE_GPT_IMAGE,$<)
endif
