# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBGIF_COMPAT_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libgif-compat.so=$(BUILD)/dynamic/libgif-compat.so)

$(eval $(call USERLAND_PACKAGE,libgif-compat,libgif-compat,*,y,library,$(LIBGIF_COMPAT_SOURCES),y,desktop,desktop/libgif-compat,,,shared-library,$(LIBGIF_COMPAT_IMAGE_DATA),,lib))
