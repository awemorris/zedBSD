.PHONY: rootfs-bin rootfs-usr
rootfs-bin: $(I386_ARCH_INPUTS)
rootfs-usr: $(ZEDBSD_PACKAGE_INPUTS)
rootfs: rootfs-bin rootfs-usr
