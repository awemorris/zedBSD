# The x86_64 kernel with every GPU driver that reports display events (venus, i915, nvrtx, amdrdna and
# ghci), for the warning-free build of the display-event fence.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the nested makes read it from their own directories.
WS353_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../..)
include $(WS353_TREE)/config/config.x86_64.mk

CONFIG_DRIVER_PCI_GHCI := y
