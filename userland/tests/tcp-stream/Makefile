include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws220-p002: a blocking TCP sender for WS220's long transfers (plan/ws220/tests/tcp-stream-series.sh): connects and
# writes a number of bytes with blocking writes, the way a plain program sends, so the kernel's send path is the one
# a blocked writer takes (sshd writes without blocking, and its cipher bounds the rate).  Test images only.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,tcp-stream,blocking TCP sender,*,n,basic,userland/tests/tcp-stream/main.c,y,tests,tests/tcp-stream,,0755,program,,,bin))
