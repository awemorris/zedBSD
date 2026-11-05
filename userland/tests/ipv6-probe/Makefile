include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws130-p002: tries the kernel's IPv6 core without a transport (the address, route and routing-socket interfaces, duplicate
# address detection, the router solicitation and advertisement) on a guest whose interface reaches a router (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,ipv6-probe,IPv6 kernel probe,x86_64,n,basic,userland/tests/ipv6-probe/main.c userland/tests/ipv6-probe/transport.c userland/tests/ipv6-probe/libc.c,y,tests,tests/ipv6-probe,,0755,program,,,bin))
