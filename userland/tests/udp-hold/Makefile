include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# A UDP socket that holds what arrives unread for a number of seconds, then counts the datagrams queued: the per-socket
# limit on queued jumbo datagrams.  Test images only.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,udp-hold,UDP receive queue probe,*,n,basic,userland/tests/udp-hold/main.c,y,tests,tests/udp-hold,,0755,program,,,bin))
