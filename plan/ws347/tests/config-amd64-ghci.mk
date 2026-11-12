# The x86_64 kernel with the GHCI 1.0 GPU driver: the platform's configuration with the driver.
# Kernel: make PLATFORM=x86_64 CONFIGMK=plan/ws347/tests/config-amd64-ghci.mk build/x86_64/vmunix
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/config.x86_64.mk

CONFIG_DRIVER_PCI_GHCI := y
