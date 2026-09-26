include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# A wl_shm test client (WS035 p053): a window drawn with the CPU.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,wlshm,Wayland wl_shm test,amd64,y,basic,userland/base/wlshm/main.c,y,base,base/wlshm,base/libwayland,0755,program,,,bin))
