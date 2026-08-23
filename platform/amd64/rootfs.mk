.PHONY: rootfs-bin rootfs-usr
rootfs-bin: $(AMD64_ARCH_INPUTS)
rootfs-usr: $(ZEDBSD_PACKAGE_INPUTS)
rootfs: rootfs-bin rootfs-usr
