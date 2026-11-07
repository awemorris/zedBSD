# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBMEDIA_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libmedia.so=$(BUILD)/dynamic/libmedia.so)

$(eval $(call USERLAND_PACKAGE,libmedia,libmedia,x86_64,n,library,$(LIBMEDIA_SOURCES),y,desktop,desktop/libmedia,,,shared-library,$(LIBMEDIA_IMAGE_DATA),,lib))
