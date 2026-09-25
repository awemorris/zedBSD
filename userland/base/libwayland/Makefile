include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Independent Wayland client library; protocol objects are maintained source.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBWAYLAND_SOURCES := userland/base/libwayland/client.c \
	userland/base/libwayland/proxy.c userland/base/libwayland/wire.c \
	userland/base/libwayland/event.c userland/base/libwayland/protocol.c \
	userland/base/libwayland/utility.c

# The public SONAME follows the standard client library spelling.
LIBWAYLAND_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libwayland-client.so=$(BUILD)/dynamic/libwayland-client.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libwayland-client,Wayland client,amd64,y,library,$(LIBWAYLAND_SOURCES),y,base,base/libwayland,,,shared-library,$(LIBWAYLAND_IMAGE_DATA),,lib))
