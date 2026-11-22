# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Toolchain
#

include mk/toolchain-llvm/llvm.mk
include mk/toolchain-gcc/gcc.mk

# The prebuilt compiler of the platform: GCC's for the platforms built
# with GCC (their defaults.mk sets PLATFORM_TOOLCHAIN := gcc), LLVM's for
# the others.
ifeq ($(PLATFORM_TOOLCHAIN),gcc)
toolchain-cached: toolchain-gcc-cached
else
toolchain-cached: toolchain-llvm-cached
endif
