# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include mk/config.mk
include mk/platform-macppc64/defaults.mk
include $(ZEDBSD_CONFIGMK)

include mk/toolchain.mk
.PHONY: toolchain-platform
toolchain-platform: toolchain-llvm

include mk/platform-macppc64/libc.mk
include mk/platform-macppc64/sysroot.mk
include mk/platform-macppc64/vmunix.mk
include mk/platform-macppc64/firmware.mk
include mk/platform-macppc64/bootloader.mk
include mk/platform-macppc64/programs.mk
include mk/platform-macppc64/rootfs.mk
include mk/platform-macppc64/image.mk

# Rebuild tracking: objects follow their flags and headers.  It refers to
# the object lists and the flags the files above define, so it comes last.
include mk/tracking.mk
