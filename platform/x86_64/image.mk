DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

.PHONY: image
image: rootfs $(DISK_IMAGE_ARTIFACT)
