# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

KEILAND_PRINTD_SOURCES := \
	userland/desktop/printd/main.c \
	userland/desktop/printd/spool.c \
	userland/desktop/printd/net.c \
	userland/desktop/printd/ipp.c \
	userland/desktop/printd/lpd.c

$(eval $(call USERLAND_PACKAGE,keiland-printd,keiland-printd - Printer daemon,*,n,basic,$(KEILAND_PRINTD_SOURCES),y,desktop,,,0755,program,,,usr/libexec))
