# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include mk/toolchain.mk
.PHONY: toolchain-platform
toolchain-platform: toolchain-llvm

include mk/platform-pc9801ns/defaults.mk
include mk/platform-pc9801ns/libc.mk
include mk/platform-pc9801ns/sysroot.mk
include mk/platform-pc9801ns/vmunix.mk
include mk/platform-pc9801ns/firmware.mk
include mk/platform-pc9801ns/bootloader.mk
include mk/platform-pc9801ns/programs.mk
include mk/platform-pc9801ns/rootfs.mk
include mk/platform-pc9801ns/image.mk

# Rebuild tracking: objects follow their flags and headers.  It refers to
# the object lists and the flags the files above define, so it comes last.
include mk/tracking.mk
