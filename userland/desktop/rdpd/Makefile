# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

RDPD_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(RDPD_DIR)../librdp/sources.mk
include $(RDPD_DIR)../package.mk

RDPD_SOURCES := \
	userland/desktop/rdpd/main.c \
	userland/desktop/rdpd/hid.c \
	userland/desktop/rdpd/session.c \
	userland/desktop/wayland/rdp-headless-proto.c \
	$(LIBRDP_SOURCES)

RDPD_DATA := \
	/etc/service.d/rdpd=userland/desktop/rdpd/rdpd.service

$(eval $(call USERLAND_PACKAGE,rdpd,rdpd - Remote Desktop Server Daemon,x86_64,n,basic,$(RDPD_SOURCES),y,desktop,desktop/rdpd,base/libssl-compat base/libcrypto-compat base/passkey desktop/wayland,0755,program,$(RDPD_DATA),,sbin))
