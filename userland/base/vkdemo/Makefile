include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,vkdemo,vkdemo,amd64,n,basic,userland/base/vkdemo/main.c userland/base/vkdemo/renderer.c userland/base/vkdemo/display.c userland/base/common/sha256.c,y,base,base/vkdemo,base/libvulkan,0755,program,,,bin))
