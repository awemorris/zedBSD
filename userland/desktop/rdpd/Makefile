# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../librdp/sources.mk

RDPD_SOURCES := \
	userland/desktop/rdpd/main.c \
	userland/desktop/rdpd/hid.c \
	userland/desktop/rdpd/session.c \
	$(LIBRDP_SOURCES)

$(eval $(call USERLAND_PACKAGE,rdpd,rdpd - Remote Desktop Server Daemon,x86_64,n,basic,$(RDPD_SOURCES),y,desktop,desktop/rdpd,base/libssl-compat base/libcrypto-compat base/passkey desktop/wayland,0755,program,,,sbin))
