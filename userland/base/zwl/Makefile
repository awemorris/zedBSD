include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Wayland compositor: window mode (Vulkan) and fullscreen mode (direct scanout).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZWL_SOURCES := userland/base/zwl/main.c userland/base/zwl/wire.c \
	userland/base/zwl/objects.c userland/base/zwl/protocol.c userland/base/zwl/display.c \
	userland/base/zwl/seat.c userland/base/zwl/input.c \
	userland/base/zwl/compose.c userland/base/zwl/import.c userland/base/zwl/shm.c \
	userland/base/zwl/glass.c userland/base/zwl/shell.c userland/base/zwl/home.c \
	userland/base/vkdemo/display.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,zwl,Wayland compositor,amd64,y,basic,$(ZWL_SOURCES),y,base,base/zwl,base/libvulkan base/libtruetype,0755,program,,,bin))
