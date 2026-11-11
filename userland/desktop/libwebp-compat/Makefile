# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBWEBP_COMPAT_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libwebp-compat.so=$(BUILD)/dynamic/libwebp-compat.so) \
	/usr/share/licenses/libwebp-compat/libwebp-COPYING=userland/base/licenses/libwebp-compat/libwebp-COPYING

$(eval $(call USERLAND_PACKAGE,libwebp-compat,libwebp-compat,*,y,library,$(LIBWEBP_COMPAT_SOURCES),y,desktop,desktop/libwebp-compat,,,shared-library,$(LIBWEBP_COMPAT_IMAGE_DATA),,lib))
