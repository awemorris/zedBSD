include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libpng-compat: libpng's simplified API for the desktop programs (plan/ws035/ws.md D1 to D6); no libpng code.
# This first part reads PNG files (ws071-p010); writing comes with ws035-p041.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

LIBPNG_COMPAT_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libpng-compat.so=$(BUILD)/dynamic/libpng-compat.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libpng-compat,libpng-compat,*,y,library,$(LIBPNG_COMPAT_SOURCES),y,desktop,desktop/libpng-compat,base/libz-compat,,shared-library,$(LIBPNG_COMPAT_IMAGE_DATA),,lib))
