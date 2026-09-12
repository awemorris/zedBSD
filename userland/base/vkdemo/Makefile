include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,vkdemo,vkdemo,amd64,n,basic,userland/base/vkdemo/main.c userland/base/vkdemo/renderer.c userland/gpu/venus/client.c userland/base/common/sha256.c,y,base,,,0755,program,,,bin))
