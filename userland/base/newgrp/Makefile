# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

NEWGRP_SOURCES := \
	userland/base/newgrp/main.c

$(eval $(call USERLAND_PACKAGE,newgrp,newgrp,*,y,basic,$(NEWGRP_SOURCES),y,base,,,4755))
