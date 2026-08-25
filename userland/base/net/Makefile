include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,net,net,*,y,network,userland/base/net/main.c userland/base/net/netconf.c userland/base/service/service-config.c,n,base,,,0755,program,,,sbin))
