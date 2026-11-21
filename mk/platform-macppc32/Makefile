# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include mk/toolchain.mk
.PHONY: toolchain-platform
toolchain-platform: toolchain-llvm

include mk/platform-macppc32/defaults.mk
include mk/platform-macppc32/libc.mk
include mk/platform-macppc32/sysroot.mk
include mk/platform-macppc32/vmunix.mk
include mk/platform-macppc32/firmware.mk
include mk/platform-macppc32/bootloader.mk
include mk/platform-macppc32/programs.mk
include mk/platform-macppc32/rootfs.mk
include mk/platform-macppc32/image.mk

# Rebuild tracking: objects follow their flags and headers.  It refers to
# the object lists and the flags the files above define, so it comes last.
include mk/tracking.mk
