DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

.PHONY: disk-image check-disk-image
disk-image: world bootloader $(DISK_IMAGE_ARTIFACT)

check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) platform/sparcv9/tools/check-sparcv9-hdd-image.py \
		--stage1 $(BUILD)/boot/stage1.bin --stage2 $(BUILD)/boot/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh \
		--sysctl $(BUILD)/bin/sysctl --rtld $(SPARCV9_DYNAMIC_DIR)/ld.so \
		--libc $(SPARCV9_DYNAMIC_DIR)/libc.so \
		--tlstest $(SPARCV9_DYNAMIC_DIR)/tlstest.so \
		--rpathdep $(SPARCV9_DYNAMIC_DIR)/alt/rpathdep.so \
		--rpathtest $(SPARCV9_DYNAMIC_DIR)/rpathtest.so \
		--verstest $(SPARCV9_DYNAMIC_DIR)/verstest.so \
		--versuse $(SPARCV9_DYNAMIC_DIR)/versuse.so \
		--dyntest $(SPARCV9_DYNAMIC_DIR)/dyntest $<
