include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Explicitly selected finite fixture; private helpers stay outside the public DSO ABI.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,gpu-share-test,GPU allocation sharing test,x86_64,y,basic,userland/tests/gpu-share/main.c,y,tests,tests/gpu-share,desktop/libwayland,0755,program,,,bin))
