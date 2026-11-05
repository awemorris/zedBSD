include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package: fpgareg, which reads and writes the registers
# of the block the programmable logic holds (/dev/fpga0) and waits for its
# interrupt.  Root's, as the device is.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,fpgareg,fpgareg,*,n,basic,userland/base/fpgareg/main.c,y,base,,,0755,program,,,sbin))
