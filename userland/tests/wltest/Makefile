include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Portable standard Wayland/Vulkan presentation test.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
WLTEST_SOURCES := userland/tests/wltest/main.c userland/tests/wltest/window.c \
	userland/tests/wltest/renderer.c
$(eval $(call USERLAND_PACKAGE,wltest,Wayland Vulkan test,amd64,y,basic,$(WLTEST_SOURCES),y,tests,tests/wltest,desktop/libvulkan desktop/libwayland,0755,program,,,bin))
