include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# A greeter without a picture for sessiond's tests (WS035 p094).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
GREETER_PROBE_SOURCES := userland/tests/greeter-probe/main.c
$(eval $(call USERLAND_PACKAGE,greeter-probe,sessiond greeter probe,x86_64,n,basic,$(GREETER_PROBE_SOURCES),y,tests,tests/greeter-probe,,0755,program,,,bin))
