include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# A client of libkeiland's system for the tests of WS131 p010: prints what the compositor offers and every state,
# asks the network, the sound, the power and the devices, and prints each answer and change on a line of its own.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KEILAND_SYSTEM_PROBE_SOURCES := userland/tests/keiland-system/main.c
$(eval $(call USERLAND_PACKAGE,keiland-system,System probe,amd64,n,basic,$(KEILAND_SYSTEM_PROBE_SOURCES),y,tests,tests/keiland-system,desktop/libwayland desktop/libkeiland,0755,program,,,bin))
