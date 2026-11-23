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

# The installer's helper in /usr/libexec/, root's alone (the installer, run as root, starts it).
INSTALLER_HELPER_SOURCES := \
	userland/desktop/installer/admin/main.c \
	userland/desktop/installer/admin/index.c \
	userland/desktop/installer/admin/release.c \
	userland/desktop/installer/admin/payload.c \
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

# The admin action that starts the installer as root once the person allows it (klsudo installer).
INSTALLER_DATA := \
	/etc/keiland/admin.d/installer=userland/desktop/installer/installer.action

$(eval $(call USERLAND_PACKAGE,installer-helper,installer-helper,x86_64,n,basic,$(INSTALLER_HELPER_SOURCES),n,installer,,,0500,program,,,usr/libexec))

$(eval $(call USERLAND_PACKAGE,installer,installer - Install zedBSD,x86_64,n,basic,$(INSTALLER_SOURCES),n,installer,desktop/installer,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype,0755,program,$(INSTALLER_DATA),,bin))
