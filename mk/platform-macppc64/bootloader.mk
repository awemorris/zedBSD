# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PowerPC Mac (64-bit) boot loader rules
#
# The kernel is handed to the firmware as an ELF image; no loader is
# built.
#

.PHONY: bootloader-platform
bootloader-platform:
