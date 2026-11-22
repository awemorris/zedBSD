# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include mk/config.mk
include mk/platform-i386/defaults.mk
include $(ZEDBSD_CONFIGMK)

# The PCI bus of the SoC has functions at every slot and fixed BARs.
KERN_CPPFLAGS += -DPCI_INTEL_MID_QUIRKS

include mk/toolchain-llvm/llvm.mk
.PHONY: toolchain-platform toolchain-cached-platform
toolchain-platform: toolchain-llvm
toolchain-cached-platform: toolchain-llvm-cached

include mk/platform-i386/defaults.mk
include mk/platform-i386/libc.mk
include mk/platform-i386/sysroot.mk
include mk/platform-i386/vmunix.mk
include mk/platform-i386/firmware.mk
include mk/platform-i386/bootloader.mk
include mk/platform-i386/programs.mk
include mk/platform-i386/rootfs.mk
include mk/platform-i386/image.mk

# Rebuild tracking: objects follow their flags and headers.  It refers to
# the object lists and the flags the files above define, so it comes last.
include mk/tracking.mk
