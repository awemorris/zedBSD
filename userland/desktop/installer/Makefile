include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# install-admin: the installer's helper (userland/desktop/installer/admin), set-user-ID root, mode 4550 (owner root,
# group wheel), in /usr/libexec (outside PATH).  The menu installer is not selectable: only make installer-image
# builds it, into the installer's image alone (platform/amd64/installer-image.mk), for it writes disks as root
# without asking for a password.  It reads the units with libz-compat's inflate, linked in.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
INSTALL_ADMIN_SOURCES := userland/desktop/installer/admin/main.c userland/desktop/installer/admin/index.c \
	userland/desktop/installer/admin/unpack.c userland/desktop/installer/admin/plan.c \
	userland/desktop/installer/admin/target.c userland/desktop/installer/admin/probe.c \
	userland/desktop/installer/admin/session.c userland/base/common/account.c userland/base/common/sha256.c \
	userland/base/diskpart/table.c userland/base/libz-compat/inflate.c userland/base/libz-compat/checksum.c
$(eval $(call USERLAND_PACKAGE,install-admin,install-admin,amd64,n,basic,$(INSTALL_ADMIN_SOURCES),n,installer,,,4550,program,,,usr/libexec))

# installer: the installer's window (userland/desktop/installer), a Keiland application in the live root of the
# installer's image alone, like its helper (menu installer, not selectable).  It runs as the session's user and asks
# install-admin for everything it reads of the disks and writes to them; platform/amd64/installer-image.mk links it.
INSTALLER_SOURCES := userland/desktop/installer/main.c userland/desktop/installer/helper.c \
	userland/desktop/installer/catalog.c userland/desktop/installer/disk.c userland/desktop/installer/flow.c \
	userland/desktop/installer/screens.c userland/desktop/artwork/mark.c
$(eval $(call USERLAND_PACKAGE,installer,installer - Install zedBSD,amd64,n,basic,$(INSTALLER_SOURCES),n,installer,,,0755,program,,,bin))
