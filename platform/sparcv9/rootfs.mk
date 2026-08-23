.PHONY: rootfs-bin rootfs-usr
rootfs-bin: $(SPARCV9_ROOTFS_INPUTS)
rootfs-usr: $(ZEDBSD_PACKAGE_INPUTS)
rootfs: rootfs-bin rootfs-usr
