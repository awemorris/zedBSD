include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws173-p002: writes the compositor's composited output as a PNG (test images built with ZEDBSD_TEST_SCREEN_CAPTURE=y).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,keiland-shot,compositor screen capture,amd64 rpi4 rpi5 visionfive2,n,basic,userland/tests/keiland-shot/main.c,y,tests,tests/keiland-shot,,0755,program,,,bin))
