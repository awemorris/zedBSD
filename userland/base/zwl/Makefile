include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Minimal fullscreen Wayland compositor with independent shared GPU imports.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZWL_SOURCES := userland/base/zwl/main.c userland/base/zwl/wire.c \
	userland/base/zwl/objects.c userland/base/zwl/protocol.c userland/base/zwl/display.c \
	userland/base/zwl/seat.c userland/base/zwl/input.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,zwl,Wayland fullscreen compositor,amd64,y,basic,$(ZWL_SOURCES),y,base,base/zwl,,0755,program,,,bin))
