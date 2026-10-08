include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws143-p005: finds an input device's node /dev/input/eventN by its bus, name or place, says what it is, and prints its
# events as lines until it goes (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,evdev-probe,input device node probe,*,n,basic,userland/tests/evdev-probe/main.c,y,tests,tests/evdev-probe,,0755,program,,,bin))
