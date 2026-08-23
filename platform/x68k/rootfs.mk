.PHONY: rootfs-bin rootfs-usr
rootfs-bin: $(BUILD)/bin/sh $(USER_BASIC_TARGETS)
rootfs-usr: $(ZEDBSD_PACKAGE_INPUTS)
rootfs: rootfs-bin rootfs-usr
