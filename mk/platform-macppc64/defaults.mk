# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PowerPC Mac (64-bit) defaults
#
# The PowerMac G5 with the PowerPC 970: big-endian, the ELFv2 ABI.  The
# kernel's binding publishes no boot device and its console is the HAL's,
# so no bus or device driver is selected; the file systems, the network
# stack and the input core are the ones every kernel carries.
#

override ARCHITECTURE             := ppc64
override BOARD                    := macppc64

CONFIG_ROOTFS_DEVELOPMENT         := y
CONFIG_BOOT_SPLASH                := n
CONFIG_QUIET_BOOT                 := n
CONFIG_GRAPHICAL_LOGIN            := n

CONFIG_DRIVER_LOOP                := y
CONFIG_DRIVER_RAMDISK             := y
CONFIG_DRIVER_FS_UFS              := y
CONFIG_DRIVER_FS_FAT              := y
CONFIG_DRIVER_FS_OVERLAYFS        := y
CONFIG_KERN_NET                   := y
CONFIG_KERN_NET_IPV4              := y
CONFIG_KERN_NET_IPV6              := y
CONFIG_KERN_NET_ETHERNET          := y
CONFIG_KERN_NET_WIFI              := y
CONFIG_DRIVER_HID                 := y
