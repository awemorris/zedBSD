# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

# The 68040 Macintosh (Quadra 630 / LC 630 family; the emulator's q800).

override ARCHITECTURE             := m68k
override BOARD                    := mac68k

# The compiler of this platform is GCC (mk/gcc/), not the project's LLVM.
override PLATFORM_TOOLCHAIN       := gcc

CONFIG_ROOTFS_DEVELOPMENT         := y
CONFIG_BOOT_SPLASH                := n
CONFIG_QUIET_BOOT                 := n
CONFIG_GRAPHICAL_LOGIN            := n

# The smallest set the kernel's common code links with.
CONFIG_DRIVER_LOOP                := y
CONFIG_DRIVER_FS_UFS              := y
CONFIG_DRIVER_FS_FAT              := y
CONFIG_DRIVER_FS_OVERLAYFS        := y
CONFIG_KERN_NET                   := y
CONFIG_KERN_NET_IPV4              := y
CONFIG_KERN_NET_IPV6              := y
CONFIG_KERN_NET_ETHERNET          := y
CONFIG_KERN_NET_WIFI              := y
CONFIG_DRIVER_HID                 := y

CONFIG_USERLAND_PROGRAMS          += \
	cat \
	echo \
	login \
	ls \
	sh \
	uname
