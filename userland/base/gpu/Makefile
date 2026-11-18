# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# /sbin/gpu: lists the GPUs, their capabilities, displays, Vulkan, GL and
# video, and controls their displays, reset and reports (root only).  The
# desktop's libraries are opened at run time, so it links libc alone.
#

PROG := gpu
SRCS := main.c output.c node.c display.c session.c vulkan.c gl.c video.c service.c
CLASS := basic
INSTALL_DIR := sbin

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk
