include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,dhcpc,dhcpc,*,y,network,userland/base/dhcpc/main.c userland/base/dhcpc/inet6.c userland/base/net/dhcp6.c,y,base,,,0755,program,,,sbin))
