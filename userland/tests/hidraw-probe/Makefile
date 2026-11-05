include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws161-p002: tries a security key's raw HID node, /dev/input/hidrawN (the test kernel's loopback key, or a real one):
# the requests, two opens, CTAPHID INIT, a PING over several packets, WINK, an unknown command (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,hidraw-probe,raw HID security key probe,amd64,n,basic,userland/tests/hidraw-probe/main.c,y,tests,tests/hidraw-probe,,0755,program,,,bin))
