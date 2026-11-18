# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

USER_CPPFLAGS := \
	$(CPPFLAGS) \
	-Iinclude \
	-Isrc \
	-I. \
	-D__ZEDBSD__ \
	-DHAL_ARCH_ARM64 \
	-DKERN_USER_ABI_AARCH64 \
	-DKERN_USER_ABI_LP64

USER_CFLAGS := \
	$(CFLAGS) \
	-march=armv8-a \
	-mno-outline-atomics \
	-fno-pic \
	-fno-pie \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-fno-builtin \
	-fno-common \
	-ffunction-sections \
	-fdata-sections \
	-Os \
	-Wall \
	-Wextra \
	-Werror
