# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Intel MID 64 bit defaults: the x86_64 defaults without the desktop GPU
# drivers and their firmware, with the PowerVR G6430 driver and firmware.
#

include mk/platform-x86_64/defaults.mk

CONFIG_DRIVER_PCI_I915            := n
CONFIG_DRIVER_PCI_NVRTX           := n
CONFIG_DRIVER_PCI_AMDRDNA         := n
CONFIG_DRIVER_PCI_VENUS           := n
CONFIG_DRIVER_INTELMID64_GPU_VPU  := y

CONFIG_USERLAND_PROGRAMS := \
	$(filter-out \
		i915-firmware \
		intelax211-firmware \
		intelbt-firmware \
		nvidia-gsp-firmware \
		amd-navi-firmware, \
		$(CONFIG_USERLAND_PROGRAMS)) \
	intelmid64-powervr-firmware
