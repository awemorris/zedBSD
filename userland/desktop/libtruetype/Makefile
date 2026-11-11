# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBTRUETYPE_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libtruetype.so=$(BUILD)/dynamic/libtruetype.so)

$(eval $(call USERLAND_PACKAGE,libtruetype,libtruetype,*,y,library,$(LIBTRUETYPE_SOURCES),y,desktop,desktop/libtruetype,,,shared-library,$(LIBTRUETYPE_IMAGE_DATA),,lib))
