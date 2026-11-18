# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Intel MID 64 bit (Moorefield, Merrifield) on the edk2-intel-mid UEFI.
#
# The machine is an x86_64 PC/AT-like board without the legacy BIOS, so the
# rules are the x86_64 ones. Only the defaults, the kernel link layout and
# the PCI quirk define differ.
#

# The PCI bus of the SoC has functions at every slot and fixed BARs.
KERN_CPPFLAGS += -DPCI_INTEL_MID_QUIRKS

include mk/platform-intelmid64/defaults.mk
include mk/platform-x86_64/libc.mk
include mk/platform-x86_64/sysroot.mk
include mk/platform-x86_64/vmunix.mk
include mk/platform-x86_64/firmware.mk
include mk/platform-x86_64/bootloader.mk
include mk/platform-x86_64/programs.mk
include mk/platform-x86_64/rootfs.mk
include mk/platform-x86_64/image.mk
