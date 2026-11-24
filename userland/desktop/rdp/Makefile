# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../librdp/sources.mk

RDP_SOURCES := \
	userland/desktop/rdp/main.c \
	userland/desktop/rdp/rdp-client.c \
	userland/desktop/rdp/input.c \
	$(LIBRDP_SOURCES)

$(eval $(call USERLAND_PACKAGE,rdp,rdp - Remote Desktop Client,x86_64,n,basic,$(RDP_SOURCES),y,desktop,desktop/rdp,desktop/libmedia desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype base/libssl-compat base/libcrypto-compat,0755,program,,,bin))
