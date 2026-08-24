include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,networkd,networkd,*,y,network,userland/base/networkd/main.c userland/base/service/service-config.c,n,base,,,0755,program,,,sbin))
