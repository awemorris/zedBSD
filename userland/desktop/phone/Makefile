include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Phone (WS170): the application that puts a person's messages and calls in one timeline; the contacts and their
# timelines are kept under ~/Documents/Phone, and messages and calls go through the compositor's phone.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PHONE_SOURCES := userland/desktop/phone/main.c userland/desktop/phone/view.c userland/desktop/phone/store.c userland/desktop/phone/phonebook.c userland/desktop/phone/media.c userland/desktop/phone/receive.c \
	userland/desktop/libmms/mms.c userland/desktop/photos/library.c userland/desktop/photos/snapshot.c userland/desktop/photos/cache-folders.c \
	userland/desktop/photos/decode.c userland/desktop/preview/client.c userland/desktop/preview/zedbsd/spawn.c
# ws157-p015: the photos' pictures are decoded by keiland-preview in a sandbox, never in Phone's process.
$(eval $(call USERLAND_PACKAGE,phone,phone - Phone,x86_64,n,basic,$(PHONE_SOURCES),y,desktop,desktop/phone,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype desktop/preview base/libz-compat,0755,program,,,bin))
