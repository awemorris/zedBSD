include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Wayland compositor: window mode (Vulkan) and fullscreen mode (direct scanout).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZDESKTOP_SOURCES := userland/base/zdesktop/main.c userland/base/zdesktop/wire.c \
	userland/base/zdesktop/objects.c userland/base/zdesktop/protocol.c userland/base/zdesktop/display.c \
	userland/base/zdesktop/seat.c userland/base/zdesktop/input.c \
	userland/base/zdesktop/compose.c userland/base/zdesktop/import.c userland/base/zdesktop/shm.c \
	userland/base/zdesktop/glass.c userland/base/zdesktop/shell.c userland/base/zdesktop/home.c \
	userland/base/vkdemo/display.c
# App Home starts X11 applications through zdesktop-x11, which starts Xzed --rootless when it is needed.
ZEDBSD_PACKAGE_INPUTS += userland/base/zdesktop/zdesktop-x11
ZEDBSD_PACKAGE_FILES += $(if $(filter zdesktop,$(ZEDBSD_USER_PROGRAMS)),--file /usr/libexec/zdesktop-x11=userland/base/zdesktop/zdesktop-x11)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,zdesktop,Wayland compositor (zdesktop),amd64,y,basic,$(ZDESKTOP_SOURCES),y,base,base/zdesktop,base/libvulkan base/libtruetype,0755,program,,,bin))
