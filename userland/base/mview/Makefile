MVIEW_PACKAGE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Wayland/Vulkan model viewer and the converted model it shows by default.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
MVIEW_SOURCES := userland/base/mview/main.c userland/base/mview/window.c \
	userland/base/mview/renderer.c userland/base/mview/model.c \
	userland/base/mview/camera.c userland/base/mview/input.c
MVIEW_TEXTURES := $(sort $(notdir $(wildcard $(MVIEW_PACKAGE_DIR)models/qs40/tex/*.pam)))
MVIEW_DATA := /usr/share/mview/qs40/model.txt=userland/base/mview/models/qs40/model.txt \
	$(foreach texture,$(MVIEW_TEXTURES),\
 /usr/share/mview/qs40/tex/$(texture)=userland/base/mview/models/qs40/tex/$(texture))
$(eval $(call ZEDBSD_USERLAND_PACKAGE,mview,Wayland model viewer,amd64,y,basic,$(MVIEW_SOURCES),y,base,base/mview,base/libvulkan base/libwayland,0755,program,$(MVIEW_DATA),,bin))
