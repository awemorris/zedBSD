# The VisionFive 2 kernel with the GHCI 1.0 GPU driver, for the KC705 board in the M.2 slot (PCIe1):
# the platform's configuration with the driver polled (the PLDA root complex takes no MSI and the
# board's bitstream has no INTx pin) and host memory through the cached direct map.
# Kernel: make PLATFORM=vf2 CONFIGMK=plan/ws347/tests/config-vf2-ghci.mk build/vf2/vmunix
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/config.visionfive2.mk

PLATFORM := vf2
CONFIG_DRIVER_PCI_GHCI := y
CONFIG_GHCI_POLL := y
CONFIG_GHCI_HOST_UNCACHED := n
