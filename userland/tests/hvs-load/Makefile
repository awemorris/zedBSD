include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws214-p003b: loads the HVS with overlay planes (GPU_DISPLAY_COMMIT) to measure underruns on a Pi (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,hvs-load,HVS overlay load probe,x86_64,n,basic,userland/tests/hvs-load/main.c,y,tests,tests/hvs-load,,0755,program,,,bin))
