# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include mk/toolchain-llvm/llvm.mk
.PHONY: toolchain-platform toolchain-cached-platform
toolchain-platform: toolchain-llvm
toolchain-platform-cached: toolchain-llvm-cached

include mk/platform-vf2/defaults.mk
include mk/platform-vf2/libc.mk
include mk/platform-vf2/sysroot.mk
include mk/platform-vf2/vmunix.mk
include mk/platform-vf2/firmware.mk
include mk/platform-vf2/bootloader.mk
include mk/platform-vf2/programs.mk
include mk/platform-vf2/rootfs.mk
include mk/platform-vf2/image.mk

# Rebuild tracking: objects follow their flags and headers.  It refers to
# the object lists and the flags the files above define, so it comes last.
include mk/tracking.mk
