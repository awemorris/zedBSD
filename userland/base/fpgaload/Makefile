# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# programmable logic (/dev/fpga0) and enables or disables it.  Root's, as
# the device is; a configuration that names it puts it into its root (the
# ZYBO's test root).  fpgareg (userland/base/fpgareg) reaches the block.
#

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

FPGALOAD_SOURCES := \
	userland/base/fpgaload/main.c \
	userland/base/fpgaload/bitstream.c

$(eval $(call USERLAND_PACKAGE,fpgaload,fpgaload,*,n,basic,$(FPGALOAD_SOURCES),y,base,,,0755,program,,,sbin))
