include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libwebp-compat: the decoding part of libwebp's interface for the desktop programs and the browser (header
# include/libc/compat/webp/decode.h); no libwebp code.  Still images: lossy, lossless, alpha, RIFF and VP8X.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

LIBWEBP_COMPAT_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libwebp-compat.so=$(BUILD)/dynamic/libwebp-compat.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libwebp-compat,libwebp-compat,*,y,library,$(LIBWEBP_COMPAT_SOURCES),y,desktop,desktop/libwebp-compat,,,shared-library,$(LIBWEBP_COMPAT_IMAGE_DATA),,lib))
