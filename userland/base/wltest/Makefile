include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Portable standard Wayland/Vulkan presentation test.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
WLTEST_SOURCES := userland/base/wltest/main.c userland/base/wltest/window.c \
	userland/base/wltest/renderer.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,wltest,Wayland Vulkan test,amd64,y,basic,$(WLTEST_SOURCES),y,base,base/wltest,base/libvulkan base/libwayland,0755,program,,,bin))
