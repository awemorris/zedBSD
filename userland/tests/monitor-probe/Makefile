include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The System Monitor's backend area on zedBSD without the compositor (WS134 p008): opens
# libkeiland-backend-zedbsd's monitor, prints its info and samples for the guest test
# (plan/ws134/tests/monitor-backend-p008.sh).  Only for test images.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
MONITOR_PROBE_SOURCES := userland/tests/monitor-probe/main.c \
	userland/desktop/libkeiland-backend-zedbsd/monitor-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/network-link-zedbsd.c \
	userland/base/net/protocol.c userland/base/net/wifi-conf.c userland/base/net/wifi-store.c userland/base/net/netconf.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,monitor-probe,System Monitor backend probe,amd64,n,basic,$(MONITOR_PROBE_SOURCES),y,tests,tests/monitor-probe,,0755,program,,,bin))
