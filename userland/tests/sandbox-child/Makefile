include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws168-p002: the static child of the sandbox test (sandboxtest starts it with sandbox_spawn; test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,sandbox-child,Sandbox test child,x86_64,n,static,userland/tests/sandbox-child/main.c,y,tests,tests/sandbox-child,,0755,program,,,usr/libexec))
