# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

$(eval $(call USERLAND_PACKAGE,cflow,cflow,*,y,basic,userland/base/cflow/main.c userland/base/common/c_parser.c,y,base,,,0755,program,,,lib/debug))
