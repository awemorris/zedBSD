include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws132-p004: talks to volumed (list, mount, eject) for the tests (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,volumectl,volumed client probe,*,n,basic,userland/tests/volumectl/main.c,y,tests,tests/volumectl,,0755,program,,,bin))
