# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

.PHONY: firmware-platform
firmware-platform:

.PHONY: firmware-licenses-platform
firmware-licenses-platform:
	$(MAKE) -f mk/userland.mk firmware-licenses
