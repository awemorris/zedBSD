include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,ntpdate,ntpdate,*,y,basic,userland/base/ntpdate/main.c userland/base/service/service-config.c,y,base,,,0755,program,,,sbin))
