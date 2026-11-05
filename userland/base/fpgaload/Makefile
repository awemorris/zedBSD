include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package: fpgaload, which writes a bitstream into the
# programmable logic (/dev/fpga0) and enables or disables it.  Root's, as
# the device is; a configuration that names it puts it into its root (the
# ZYBO's test root).  fpgareg (userland/base/fpgareg) reaches the block.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,fpgaload,fpgaload,*,n,basic,userland/base/fpgaload/main.c userland/base/fpgaload/bitstream.c,y,base,,,0755,program,,,sbin))
