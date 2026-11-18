# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

COMMON_SOURCES := \
	userland/base/common/account.c \
	userland/base/common/archive.c \
	userland/base/common/builtin-standalone.c \
	userland/base/common/c_parser.c \
	userland/base/common/command.c \
	userland/base/common/elf_symbols.c \
	userland/base/common/format-file.c \
	userland/base/common/lzw.c \
	userland/base/common/pager.c \
	userland/base/common/sccs.c \
	userland/base/common/sha256.c \
	userland/base/common/terminfo.c \
	userland/base/common/uucodec.c

$(eval $(call USERLAND_PACKAGE,common,common,*,n,library,$(COMMON_SOURCES),n,base,,))
