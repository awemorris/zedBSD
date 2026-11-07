# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Owns the recoverable photo/video database; the compositor invokes this CLI.

MEDIASTORAGE_SOURCES := \
	userland/desktop/mediastorage/main.c \
	userland/desktop/mediastorage/notify.c \
	userland/desktop/mediastorage/database.c \
	userland/desktop/mediastorage/json.c \
	userland/desktop/photos/cache-folders.c \
	userland/desktop/photos/import.c \
	userland/desktop/photos/exif.c \
	userland/desktop/photos/library.c \
	userland/desktop/photos/snapshot.c \
	userland/desktop/preview/client.c \
	userland/desktop/preview/zedbsd/spawn.c

$(eval $(call USERLAND_PACKAGE,mediastorage,mediastorage - Media library CLI,x86_64,n,basic,$(MEDIASTORAGE_SOURCES),y,desktop,desktop/mediastorage,desktop/libkeiland desktop/libwayland desktop/preview,0755,program,,,bin))
