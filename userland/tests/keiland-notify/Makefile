include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# A client of the desktop's notifications for the tests of WS156 p002: posts a notification through libkeiland's
# kl_system_notify and prints its number and every event of it on a line of its own.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KEILAND_NOTIFY_SOURCES := userland/tests/keiland-notify/main.c
$(eval $(call USERLAND_PACKAGE,keiland-notify,Notification probe,x86_64,n,basic,$(KEILAND_NOTIFY_SOURCES),y,tests,tests/keiland-notify,desktop/libwayland desktop/libkeiland,0755,program,,,bin))
