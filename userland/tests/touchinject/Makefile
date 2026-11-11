include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The test touch screen's driver and reader (WS079 p012): replays touch scripts
# through /dev/input-inject, checks its refusals and dumps its evdev node.  Only
# for test images whose kernel has CONFIG_INPUT_TEST_INJECT=y.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
TOUCHINJECT_SOURCES := userland/tests/touchinject/main.c
TOUCHINJECT_DATA := /usr/share/touchinject/two-fingers.touch=userland/tests/touchinject/two-fingers.touch
$(eval $(call USERLAND_PACKAGE,touchinject,test touch screen injector,*,n,basic,$(TOUCHINJECT_SOURCES),y,tests,tests/touchinject,,0755,program,$(TOUCHINJECT_DATA),,bin))
