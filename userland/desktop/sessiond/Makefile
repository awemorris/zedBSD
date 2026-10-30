# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

# The graphical login's session manager (plan/ws035/login-manager-design.md): the service "greeter", which
# replaces the console's getty when the boot parameters ask for the graphical login (login=graphical).
# It checks every login, unlock and PIN change with /sbin/passkey (ws172-p002), so it requires base/passkey: an image
# with sessiond gets passkey whether or not its config names it.
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SESSIOND_SOURCES := userland/desktop/sessiond/main.c userland/desktop/sessiond/greeter.c \
	userland/desktop/sessiond/session.c userland/desktop/sessiond/seat.c userland/desktop/sessiond/auth.c \
	userland/desktop/sessiond/auth-policy.c userland/desktop/sessiond/power.c userland/desktop/sessiond/power-rules.c userland/desktop/sessiond/sleep.c userland/desktop/sessiond/sleep-rules.c \
	userland/desktop/sessiond/service.c userland/desktop/sessiond/service-rules.c userland/desktop/sessiond/thermal.c userland/desktop/sessiond/thermal-rules.c userland/desktop/sessiond/fallback-rules.c userland/base/common/account.c \
	userland/base/service/rcconf.c userland/base/service/service-config.c userland/base/service/zsv1-client.c \
	userland/base/service/zsv1-protocol.c userland/base/net/protocol.c
SESSIOND_DATA := /etc/service.d/greeter=userland/desktop/sessiond/greeter.service \
	/etc/keiland/session=userland/desktop/sessiond/session.sh \
	/etc/keiland/autologin=userland/desktop/sessiond/autologin
$(eval $(call ZEDBSD_USERLAND_PACKAGE,sessiond,sessiond - Graphical login session manager,amd64,y,basic,$(SESSIOND_SOURCES),y,desktop,desktop/sessiond,base/passkey,0755,program,$(SESSIOND_DATA),,sbin))
