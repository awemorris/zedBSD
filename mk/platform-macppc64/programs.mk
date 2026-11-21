# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include mk/userland.mk

USER_CPPFLAGS := \
	$(CPPFLAGS) \
	-Iinclude \
	-Isrc \
	-I. \
	-D__ZEDBSD__ \
	-DHAL_ARCH_PPC64 \
	-DKERN_USER_ABI_PPC64 \
	-DKERN_USER_ABI_LP64

USER_CFLAGS := \
	$(CFLAGS)\
	-mcpu=970 \
	-mno-altivec \
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
