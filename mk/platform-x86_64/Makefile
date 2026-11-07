include mk/platform-x86_64/defaults.mk
include mk/platform-x86_64/libc.mk
include mk/platform-x86_64/sysroot.mk
include mk/platform-x86_64/vmunix.mk
include mk/platform-x86_64/firmware.mk
include mk/platform-x86_64/bootloader.mk
include mk/platform-x86_64/programs.mk
include mk/platform-x86_64/rootfs.mk
include mk/platform-x86_64/image.mk

.PHONY: libc-platform sysroot-platform vmunix-platform rootfs-platform image-platform
.PHONY: bootloader-x86_64 firmware-x86_64

libc-platform: libc-x86_64
sysroot-platform: sysroot-x86_64
vmunix-platform: vmunix-x86_64
rootfs-platform: rootfs-x86_64
image-platform: image-x86_64

bootloader-x86_64: bootloader-platform
firmware-x86_64: firmware-platform
