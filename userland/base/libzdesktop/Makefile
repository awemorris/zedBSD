include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The desktop's single way into the operating system, other than Vulkan and
# Wayland.  Empty but for its version until a feature needs the system.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBZDESKTOP_SOURCES := userland/base/libzdesktop/version.c

LIBZDESKTOP_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libzdesktop.so=$(BUILD)/dynamic/libzdesktop.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libzdesktop,Desktop system library,amd64,y,library,$(LIBZDESKTOP_SOURCES),y,base,base/libzdesktop,,,shared-library,$(LIBZDESKTOP_IMAGE_DATA),,lib))
