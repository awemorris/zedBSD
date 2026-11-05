MVIEW_PACKAGE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Wayland/Vulkan model viewer and the converted model it shows by default.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
MVIEW_SOURCES := userland/tests/mview/main.c userland/tests/mview/window.c \
	userland/tests/mview/renderer.c userland/tests/mview/model.c \
	userland/tests/mview/camera.c userland/tests/mview/input.c
MVIEW_TEXTURES := $(sort $(notdir $(wildcard $(MVIEW_PACKAGE_DIR)models/qs40/tex/*.pam)))
MVIEW_DATA := /usr/share/mview/qs40/model.txt=userland/tests/mview/models/qs40/model.txt \
	$(foreach texture,$(MVIEW_TEXTURES),\
 /usr/share/mview/qs40/tex/$(texture)=userland/tests/mview/models/qs40/tex/$(texture))
$(eval $(call USERLAND_PACKAGE,mview,Wayland model viewer,amd64,y,basic,$(MVIEW_SOURCES),y,tests,tests/mview,desktop/libvulkan desktop/libwayland,0755,program,$(MVIEW_DATA),,bin))
