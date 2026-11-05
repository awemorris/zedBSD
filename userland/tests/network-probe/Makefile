include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# A networkd stand-in with a Wi-Fi radio, for the system bar's network menu's tests (WS035 p013).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
NETWORK_PROBE_SOURCES := userland/tests/network-probe/main.c userland/base/net/protocol.c
$(eval $(call USERLAND_PACKAGE,network-probe,networkd stand-in,amd64,n,basic,$(NETWORK_PROBE_SOURCES),y,tests,tests/network-probe,,0755,program,,,bin))
