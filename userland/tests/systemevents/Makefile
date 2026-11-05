include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The reader of the system's events for the tests of WS132 (p002): subscribes to /dev/system, prints the power's
# state and each event on a line of its own.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
SYSTEMEVENTS_SOURCES := userland/tests/systemevents/main.c
$(eval $(call USERLAND_PACKAGE,systemevents,system event reader,*,n,basic,$(SYSTEMEVENTS_SOURCES),y,tests,tests/systemevents,,0755,program,,,bin))
