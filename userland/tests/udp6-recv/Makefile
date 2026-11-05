include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# A UDP receiver on IPv6 that prints the length and a hash of each datagram it takes, for the large datagrams that
# arrive in fragments.  Test images only.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,udp6-recv,UDP over IPv6 receive probe,*,n,basic,userland/tests/udp6-recv/main.c,y,tests,tests/udp6-recv,,0755,program,,,bin))
