# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PowerPC Mac (64-bit) rootfs rules
#
# There is no 64-bit PowerPC userland to put in a root file system, so
# the target refuses rather than make an empty one.
#

.PHONY: rootfs-platform
rootfs-platform:
	@echo "macppc64: no root file system can be built for this platform" >&2
	@exit 1
