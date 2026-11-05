include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Native media verification through libmedia; test images only, without an application software fallback.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,media-probe,Native media probe,x86_64,n,basic,userland/tests/media-probe/main.c userland/base/common/sha256.c,y,tests,tests/media-probe,desktop/libmedia,0755,program,,,bin))
