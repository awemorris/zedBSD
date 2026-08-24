include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

$(eval $(call ZEDBSD_USERLAND_PACKAGE,logger,logger,*,y,basic,userland/base/deferred-stub/main.c,y,base,,))
USERLAND_logger_PROVIDER := system-logging
