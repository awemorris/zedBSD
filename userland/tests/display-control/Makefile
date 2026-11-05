include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws113-p012: drives GPU_DISPLAY_REFRESH and GPU_DISPLAY_POWER on a GPU node's first display (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,display-control,GPU display control probe,x86_64,n,basic,userland/tests/display-control/main.c,y,tests,tests/display-control,,0755,program,,,bin))
