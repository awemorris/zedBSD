# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBPNG_COMPAT_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libpng-compat.so=$(BUILD)/dynamic/libpng-compat.so)

$(eval $(call USERLAND_PACKAGE,libpng-compat,libpng-compat,*,y,library,$(LIBPNG_COMPAT_SOURCES),y,desktop,desktop/libpng-compat,base/libz-compat,,shared-library,$(LIBPNG_COMPAT_IMAGE_DATA),,lib))
