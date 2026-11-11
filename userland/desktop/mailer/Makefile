# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MAILER_SOURCES := \
	userland/desktop/mailer/main.c \
	userland/desktop/mailer/view.c \
	userland/desktop/mailer/store.c \
	userland/desktop/mailer/sync.c \
	userland/desktop/mailer/tls.c \
	userland/desktop/mailer/conn.c \
	userland/desktop/mailer/imap.c \
	userland/desktop/mailer/smtp.c \
	userland/desktop/mailer/mime.c \
	userland/desktop/mailer/jis.c \
	userland/desktop/mailer/structure.c \
	userland/desktop/mailer/compose.c \
	userland/desktop/mailer/code.c \
	userland/desktop/mailer/account.c \
	userland/desktop/mailer/secret.c

$(eval $(call USERLAND_PACKAGE,mailer,mailer - Mail,x86_64,n,basic,$(MAILER_SOURCES),y,desktop,desktop/mailer,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype,0755,program,,,bin))
