# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# of the block the programmable logic holds (/dev/fpga0) and waits for its
# interrupt.  Root's, as the device is.
#

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

FPGAREG_SOURCES := \
	userland/base/fpgareg/main.c

$(eval $(call USERLAND_PACKAGE,fpgareg,fpgareg,*,n,basic,$(FPGAREG_SOURCES),y,base,,,0755,program,,,sbin))
