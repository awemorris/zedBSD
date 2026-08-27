include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,service,service,*,y,basic,userland/base/service/main.c userland/base/service/service-command.c userland/base/service/service-config.c userland/base/service/rcconf.c userland/base/service/zsv1-protocol.c userland/base/service/zsv1-client.c,y,base,,,0755,program,,,sbin))
