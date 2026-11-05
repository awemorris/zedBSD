include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws113-p013: reads and sets /dev/backlight/backlightN like FreeBSD's backlight(8) (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,backlight-probe,backlight device probe,amd64,n,basic,userland/tests/backlight-probe/main.c,y,tests,tests/backlight-probe,,0755,program,,,bin))
