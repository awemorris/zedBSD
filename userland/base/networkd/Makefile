include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,networkd,networkd,*,y,network,userland/base/networkd/main.c userland/base/networkd/confirmed.c userland/base/networkd/wifi-child.c userland/base/networkd/managed-wlan.c userland/base/net/protocol.c userland/base/net/wifi-conf.c userland/base/net/wifi-store.c,n,base,base/net,,0755,program,,,sbin))
