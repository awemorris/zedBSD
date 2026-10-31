include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libgif-compat: giflib's high-level decoding interface for the desktop programs (ws074-p051, header
# include/libc/compat/gif_lib.h); no giflib code.  Decoding only (DGifSlurp and its companions).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

LIBGIF_COMPAT_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libgif-compat.so=$(BUILD)/dynamic/libgif-compat.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libgif-compat,libgif-compat,*,y,library,$(LIBGIF_COMPAT_SOURCES),y,desktop,desktop/libgif-compat,,,shared-library,$(LIBGIF_COMPAT_IMAGE_DATA),,lib))
