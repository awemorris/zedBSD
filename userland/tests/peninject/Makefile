include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The test pen's driver and reader (WS079 p002): replays pen scripts through
# /dev/input-inject, checks its refusals and dumps its evdev node.  Only for
# test images whose kernel has CONFIG_INPUT_TEST_INJECT=y.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PENINJECT_SOURCES := userland/tests/peninject/main.c
PENINJECT_DATA := /usr/share/peninject/stroke.pen=userland/tests/peninject/stroke.pen
$(eval $(call USERLAND_PACKAGE,peninject,test pen injector,*,n,basic,$(PENINJECT_SOURCES),y,tests,tests/peninject,,0755,program,$(PENINJECT_DATA),,bin))
