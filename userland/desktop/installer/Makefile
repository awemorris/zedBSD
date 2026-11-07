# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Installer GUI.
INSTALLER_SOURCES := \
	userland/desktop/installer/main.c \
	userland/desktop/installer/helper.c \
	userland/desktop/installer/catalog.c \
	userland/desktop/installer/disk.c \
	userland/desktop/installer/flow.c \
	userland/desktop/installer/screens.c \
	userland/desktop/artwork/mark.c

# Installer helper in /usr/libexec/
INSTALL_ADMIN_SOURCES := \
	userland/desktop/installer/admin/main.c \
	userland/desktop/installer/admin/index.c \
	userland/desktop/installer/admin/unpack.c \
	userland/desktop/installer/admin/plan.c \
	userland/desktop/installer/admin/target.c \
	userland/desktop/installer/admin/probe.c \
	userland/desktop/installer/admin/session.c \
	userland/base/common/account.c \
	userland/base/common/sha256.c \
	userland/base/diskpart/table.c \
	userland/base/libz-compat/inflate.c \
	userland/base/libz-compat/checksum.c

$(eval $(call USERLAND_PACKAGE,install-admin,install-admin,x86_64,n,basic,$(INSTALL_ADMIN_SOURCES),n,installer,,,4550,program,,,usr/libexec))

$(eval $(call USERLAND_PACKAGE,installer,installer - Install zedBSD,x86_64,n,basic,$(INSTALLER_SOURCES),n,installer,,,0755,program,,,bin))
