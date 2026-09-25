include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Installed as /usr/bin/env, the path that #! lines name.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,env,env,*,y,basic,userland/base/env/main.c,y,base,,,,,,,usr/bin))
