# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include mk/config.mk
include mk/platform-rpi4/defaults.mk
include $(ZEDBSD_CONFIGMK)

include mk/toolchain-llvm/llvm.mk
.PHONY: toolchain-platform toolchain-cached-platform
toolchain-platform: toolchain-llvm
toolchain-cached-platform: toolchain-llvm-cached

include mk/platform-rpi4/libc.mk
include mk/platform-rpi4/sysroot.mk
include mk/platform-rpi4/vmunix.mk
include mk/platform-rpi4/firmware.mk
include mk/platform-rpi4/bootloader.mk
include mk/platform-rpi4/programs.mk
include mk/platform-rpi4/rootfs.mk
include mk/platform-rpi4/image.mk

# Rebuild tracking: objects follow their flags and headers.  It refers to
# the object lists and the flags the files above define, so it comes last.
include mk/tracking.mk
