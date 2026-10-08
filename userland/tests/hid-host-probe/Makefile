include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws143-p005: makes a keyboard on /dev/hid-host and types on it, or tries the node's refusals (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,hid-host-probe,HID host node probe,amd64,n,basic,userland/tests/hid-host-probe/main.c,y,tests,tests/hid-host-probe,,0755,program,,,bin))
