include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws143-p002: tries a Bluetooth controller's HCI node, /dev/btN: its information, Intel's Read Version, HCI_Reset,
# Read Local Version and Read BD_ADDR, the kernel's reset and the counts (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,bt-probe,Bluetooth HCI node probe,amd64,n,basic,userland/tests/bt-probe/main.c,y,tests,tests/bt-probe,,0755,program,,,bin))
