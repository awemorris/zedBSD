# The riscv64 kernel with the GPU drivers of the VisionFive 2 and ghci, for the warning-free build of the
# display-event fence.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/config.vf2.mk

CONFIG_DRIVER_PCI_GHCI := y
