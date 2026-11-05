include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# /sbin/passkey (ws172-p002; docs/architecture/security.md, "Login authentication"): checks a password or a PIN, or
# changes /etc/passkey, for sessiond.  Root only, mode 0500, not set-user-ID; the C library's crypt() alone.
# Registered as base/passkey so that sessiond can require it.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PASSKEY_SOURCES := userland/base/passkey/main.c userland/base/passkey/request.c userland/base/passkey/record.c \
	userland/base/common/account.c userland/base/login/verify.c
$(eval $(call USERLAND_PACKAGE,passkey,passkey,*,y,basic,$(PASSKEY_SOURCES),y,base,base/passkey,,0500,program,,,sbin))
