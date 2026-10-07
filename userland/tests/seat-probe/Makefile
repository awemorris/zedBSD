include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The keymap, key repeat, modifiers and wl_output v4 of the compositor (WS035 p078).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
SEAT_PROBE_SOURCES := userland/tests/seat-probe/main.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,seat-probe,keyboard and output probe,amd64,y,basic,$(SEAT_PROBE_SOURCES),y,tests,tests/seat-probe,desktop/libwayland,0755,program,,,bin))
