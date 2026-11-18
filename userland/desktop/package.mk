# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

# Common standalone package interface for userland/desktop: the same as userland/base.

include $(dir $(lastword $(MAKEFILE_LIST)))../base/package.mk
