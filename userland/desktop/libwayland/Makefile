include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Independent Wayland client library; protocol objects are maintained source.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBWAYLAND_SOURCES := userland/desktop/libwayland/client.c \
	userland/desktop/libwayland/proxy.c userland/desktop/libwayland/wire.c \
	userland/desktop/libwayland/event.c userland/desktop/libwayland/protocol.c \
	userland/desktop/libwayland/utility.c \
	userland/desktop/libwayland/menu-protocol.c \
	userland/desktop/libwayland/titlebar-protocol.c \
	userland/desktop/libwayland/glass-protocol.c \
	userland/desktop/libwayland/subsurface-protocol.c \
	userland/desktop/libwayland/data-device-protocol.c \
	userland/desktop/libwayland/primary-selection-protocol.c \
	userland/desktop/libwayland/tablet-protocol.c \
	userland/desktop/libwayland/touch-protocol.c \
	userland/desktop/libwayland/text-input-protocol.c \
	userland/desktop/libwayland/input-method-protocol.c \
	userland/desktop/libwayland/virtual-keyboard-protocol.c \
	userland/desktop/libwayland/ime-status-protocol.c \
	userland/desktop/libwayland/keyboard-inset-protocol.c \
	userland/desktop/libwayland/edit-protocol.c \
	userland/desktop/libwayland/activation-protocol.c \
	userland/desktop/libwayland/content-type-protocol.c \
	userland/desktop/libwayland/theme-protocol.c

# The public SONAME follows the standard client library spelling.  The .pc
# file is for a program built on the system; the headers are the sysroot's.
LIBWAYLAND_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libwayland-client.so=$(BUILD)/dynamic/libwayland-client.so \
	/usr/lib/pkgconfig/wayland-client.pc=userland/desktop/libwayland/pkgconfig/wayland-client.pc)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libwayland-client,libwayland-client,amd64,y,library,$(LIBWAYLAND_SOURCES),y,desktop,desktop/libwayland,,,shared-library,$(LIBWAYLAND_IMAGE_DATA),,lib))

# ------------------------------------------------- the stage for packages

# A package built with Meson finds libraries through pkg-config, which looks
# only in the view of the external packages it depends on
# (userland/packages/external.mk).  This stage puts the client library, the
# EGL window library and their .pc files there under the name wayland-client,
# so such a package (GTK, WS115) depends on wayland-client like on any other.
# The headers are the sysroot's.
include userland/packages/external.mk

LIBWAYLAND_PC_DIR := userland/desktop/libwayland/pkgconfig
LIBWAYLAND_STAGE := $(ZEDBSD_EXTERNAL_WORKROOT)/wayland-client/stage
LIBWAYLAND_STAGED := $(LIBWAYLAND_STAGE)/.zedbsd-staged
LIBWAYLAND_STAGE_INPUTS := $(BUILD)/dynamic/libwayland-client.so $(BUILD)/dynamic/libwayland-egl.so \
	$(LIBWAYLAND_PC_DIR)/wayland-client.pc $(LIBWAYLAND_PC_DIR)/wayland-egl.pc

# Replaces the stage as a whole from the libraries just built.
$(LIBWAYLAND_STAGED): $(LIBWAYLAND_STAGE_INPUTS)
	@rm -rf '$(LIBWAYLAND_STAGE)'
	@mkdir -p '$(LIBWAYLAND_STAGE)/usr/lib/pkgconfig'
	@cp '$(BUILD)/dynamic/libwayland-client.so' '$(BUILD)/dynamic/libwayland-egl.so' \
		'$(LIBWAYLAND_STAGE)/usr/lib/'
	@cp '$(LIBWAYLAND_PC_DIR)/wayland-client.pc' '$(LIBWAYLAND_PC_DIR)/wayland-egl.pc' \
		'$(LIBWAYLAND_STAGE)/usr/lib/pkgconfig/'
	@touch '$@'

.PHONY: wayland-client
wayland-client: $(LIBWAYLAND_STAGED)
	@:
