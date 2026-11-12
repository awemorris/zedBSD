# The VisionFive 2 kernel with the generic GHCI 1.0 GPU driver, for the KC705 GHCI board in the M.2 slot
# (PCIe1): the board's configuration with the driver polled (the PLDA root complex takes no MSI and the
# board's bitstream has no INTx pin), host memory through the cached direct map, SSH, and the kernel's
# lines on the screen.
# Kernel:  make ZEDBSD_CONFIG=plan/ws290/tests/config-visionfive2-ghci.mk BUILD=build/<dir> build/<dir>/vmunix
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-visionfive2.mk

CONFIG_DRIVER_PCI_GHCI := y
ZEDBSD_USER_PROGRAMS += ghcictl ghci-display sudo openssl openssh

ZEDBSD_BOOT_KERNEL_MESSAGES := y
