include $(dir $(lastword $(MAKEFILE_LIST)))../../package.mk

# Explicitly selected finite fixture; private helpers stay outside the public DSO ABI.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,gpu-share-test,GPU allocation sharing test,amd64,y,basic,userland/base/tests/gpu-share/main.c,y,base,base/tests/gpu-share,base/libwayland,0755,program,,,bin))
