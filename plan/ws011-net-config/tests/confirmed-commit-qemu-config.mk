# WS011 p007 test-only amd64/PC-AT confirmed-commit configuration.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZEDBSD_MENU_VERSION := 3
ZEDBSD_PLATFORM := amd64
ZEDBSD_ARCHITECTURE := amd64
ZEDBSD_BOARD := pcat
ZEDBSD_VARIANT := hybrid

# Keep only the hardware needed by the deterministic legacy-PC cell. Essential
# init/login/network administration packages are added by the common build.
ZEDBSD_USER_PROGRAMS := cat cksum ifconfig ping route sleep

CONFIG_KERNEL_TEST_CHECKPOINTS := n
CONFIG_KERNEL_USB_HID_CHECKPOINT := n
CONFIG_BUF_CACHE_KIB := 0
CONFIG_DRIVER_NE2000 := y
CONFIG_DRIVER_PCI_UHCI := n
CONFIG_DRIVER_PCI_EHCI := n
CONFIG_DRIVER_PCI_XHCI := n
CONFIG_DRIVER_PCI_NVME := n
CONFIG_DRIVER_PCI_INTEL_AX211 := n
CONFIG_DRIVER_USB_STORAGE := n
CONFIG_DRIVER_USB_CDC_NCM := n
CONFIG_DRIVER_USB_CDC_ECM := n
CONFIG_DRIVER_USB_HID := n
CONFIG_DRIVER_USB_RTL8822BU := n
CONFIG_DRIVER_GRAPHICS := y
CONFIG_DRIVER_LGY98 := n
