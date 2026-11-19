# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Toolchain
#

include mk/llvm/llvm.mk

toolchain: llvm-toolchain

toolchain-cached: llvm-toolchain-cached
