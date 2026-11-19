# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Toolchain
#

include mk/llvm/llvm.mk
include mk/gcc/gcc.mk

toolchain-cached: toolchain-llvm-cached
