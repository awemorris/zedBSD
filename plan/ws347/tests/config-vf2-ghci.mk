# The VisionFive 2 kernel with the GHCI 1.0 GPU driver, for the KC705 board in the M.2 slot (PCIe1):
# the platform's configuration with the driver (it finds at the attach that the PLDA root complex
# offers no interrupt and polls, and whether the bus's DMA is coherent).
# Kernel: make PLATFORM=vf2 CONFIGMK=plan/ws347/tests/config-vf2-ghci.mk build/vf2/vmunix
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/config.vf2.mk

PLATFORM := vf2
CONFIG_DRIVER_PCI_GHCI := y
