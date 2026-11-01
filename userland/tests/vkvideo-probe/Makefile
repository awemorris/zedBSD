include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The Vulkan Video probe: lists a device's video decode, decodes an H.264 or H.265 stream (libmedia's H.265 parse) and
# prints each frame's SHA-256.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,vkvideo-probe,Vulkan Video probe,amd64 rpi4,n,basic,userland/tests/vkvideo-probe/main.c userland/tests/vkvideo-probe/h264.c userland/tests/vkvideo-probe/dpb.c userland/tests/vkvideo-probe/frame.c userland/tests/vkvideo-probe/h265.c userland/base/common/sha256.c userland/desktop/libmedia/h265-stream.c userland/desktop/libmedia/h265-dpb.c userland/desktop/libmedia/h265-std.c userland/desktop/libmedia/h265sw-nal.c userland/desktop/libmedia/h265sw-poc.c userland/desktop/libmedia/h265sw-ps.c userland/desktop/libmedia/h265sw-rps.c userland/desktop/libmedia/h265sw-scaling.c userland/desktop/libmedia/h265sw-slice.c userland/desktop/libmedia/h265sw-vui.c userland/desktop/libmedia/vcodec-bitreader.c userland/desktop/libmedia/vcodec-nal.c userland/desktop/libmedia/bits.c,y,tests,tests/vkvideo-probe,desktop/libvulkan,0755,program,,,bin))
