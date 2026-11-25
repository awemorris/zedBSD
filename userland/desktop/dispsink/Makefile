# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../librdp/sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

DISPSINK_SOURCES := \
	userland/desktop/dispsink/main.c \
	userland/desktop/rdp/rdp-client.c \
	userland/desktop/rdp/input.c \
	apps/mikuterm-common/netdisp-proto.c \
	$(LIBRDP_SOURCES)

$(eval $(call USERLAND_PACKAGE,dispsink,dispsink - Display Sink (Target Mode),x86_64,n,basic,$(DISPSINK_SOURCES),y,desktop,desktop/dispsink,desktop/libmedia desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype base/libssl-compat base/libcrypto-compat,0755,program,,,bin))
