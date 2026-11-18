# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Installed as /usr/bin/env, the path that #! lines name.
#

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ENV_SOURCES := \
	userland/base/env/main.c

$(eval $(call USERLAND_PACKAGE,env,env,*,y,basic,$(ENV_SOURCES),y,base,,,,,,,usr/bin))
