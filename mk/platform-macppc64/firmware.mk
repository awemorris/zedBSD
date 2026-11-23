# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PowerPC Mac (64-bit) firmware rules
#
# The machine boots from its own Open Firmware; nothing is built for it.
#

.PHONY: firmware-platform
firmware-platform:

.PHONY: firmware-licenses-platform
firmware-licenses-platform:
	$(MAKE) -f mk/userland.mk firmware-licenses PLATFORM=$(PLATFORM) BUILD=$(BUILD) ZEDBSD_CONFIGMK=$(ZEDBSD_CONFIGMK) TOPLEVEL_BUILD=$(TOPLEVEL_BUILD)
