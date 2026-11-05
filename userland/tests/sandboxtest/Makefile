include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws168-p002: the kernel's sandbox test (sandbox_spawn and the child's set of calls, with sandbox-child; test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,sandboxtest,Sandbox test,amd64,n,basic,userland/tests/sandboxtest/main.c,y,tests,tests/sandboxtest,tests/sandbox-child,0755,program,,,bin))
