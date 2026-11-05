include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# OpenGL ES 3.1 compute test (WS101 p009): runs compute shaders in a pbuffer context and checks their results.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
GLESCOMPUTE_SOURCES := userland/tests/glescompute/main.c
$(eval $(call USERLAND_PACKAGE,glescompute,OpenGL ES compute test,amd64,n,basic,$(GLESCOMPUTE_SOURCES),y,tests,tests/glescompute,desktop/libegl desktop/libglesv2,0755,program,,,bin))
