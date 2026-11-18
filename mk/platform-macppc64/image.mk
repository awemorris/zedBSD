# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PowerPC Mac (64-bit) disk image rules
#
# Without a root file system there is no disk image; the kernel is booted
# by handing its ELF to the firmware.
#

.PHONY: disk-image image-platform image-macppc64
disk-image: image-platform
image-platform:
	@echo "macppc64: no disk image can be built for this platform" >&2
	@exit 1
image-macppc64: image-platform
