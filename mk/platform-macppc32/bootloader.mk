# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PowerPC Mac (32-bit) boot loader rules
#
# Open Firmware loads the kernel's ELF itself: a New World Mac's firmware
# reads the CHRP boot script \ppc\bootinfo.txt from the HFS boot partition
# and boots \vmunix beside it.  The disk image's tool writes both
# (image.mk), so there is no loader to build.
#

.PHONY: bootloader-platform
bootloader-platform:
