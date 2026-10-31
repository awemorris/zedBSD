include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libz-compat: zedBSD's own zlib interface for the base programs (plan/ws035/ws.md D1 to D6); no zlib code.
# It decompresses (ws071-p010) and compresses (deflate.c, ws175-p006).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

LIBZ_COMPAT_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libz-compat.so=$(BUILD)/dynamic/libz-compat.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libz-compat,libz-compat,*,y,library,$(LIBZ_COMPAT_SOURCES),y,base,base/libz-compat,,,shared-library,$(LIBZ_COMPAT_IMAGE_DATA),,lib))
