include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,cksum,cksum,*,y,basic,userland/base/cksum/main.c userland/base/common/sha256.c,y,base,,))
