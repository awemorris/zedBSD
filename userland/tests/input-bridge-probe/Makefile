include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws143-p005: makes a keyboard on /dev/input/bridge and types on it, or tries the node's refusals (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,input-bridge-probe,input bridge node probe,amd64,n,basic,userland/tests/input-bridge-probe/main.c,y,tests,tests/input-bridge-probe,,0755,program,,,bin))
