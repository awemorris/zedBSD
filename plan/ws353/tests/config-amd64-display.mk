# The x86_64 kernel with every GPU driver that reports display events (venus, i915, nvrtx, amdrdna and
# ghci), for the warning-free build of the display-event fence.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/config.x86_64.mk

CONFIG_DRIVER_PCI_GHCI := y
