# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CHGRP_SOURCES := \
	userland/base/chgrp/main.c \
	userland/base/chown/change.c

$(eval $(call USERLAND_PACKAGE,chgrp,chgrp,*,y,basic,$(CHGRP_SOURCES),y,base,))
