# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include mk/toolchain-llvm/llvm.mk
.PHONY: toolchain-platform toolchain-cached-platform
toolchain-platform: toolchain-llvm
toolchain-platform-cached: toolchain-llvm-cached

include mk/platform-mac68k/defaults.mk
include mk/platform-mac68k/vmunix.mk
include mk/platform-mac68k/bootloader.mk
include mk/platform-mac68k/image.mk

# Rebuild tracking: objects follow their flags and headers.  It refers to
# the object lists and the flags the files above define, so it comes last.
include mk/tracking.mk
