# Focused WS005 p005 amd64/UEFI native credential acceptance configuration.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZEDBSD_MENU_VERSION := 3
ZEDBSD_PLATFORM := amd64
ZEDBSD_ARCHITECTURE := amd64
ZEDBSD_BOARD := pcat
ZEDBSD_VARIANT := uefi

# The probe is the init process.  Only the production net command is selected
# explicitly; its ordinary package dependencies remain Make-controlled.
ZEDBSD_USER_PROGRAMS := net

CONFIG_KERNEL_TEST_CHECKPOINTS := n
CONFIG_KERNEL_USB_HID_CHECKPOINT := n
CONFIG_BUF_CACHE_KIB := 0
CONFIG_DRIVER_NE2000 := n
CONFIG_DRIVER_PCI_UHCI := n
CONFIG_DRIVER_PCI_EHCI := n
CONFIG_DRIVER_PCI_XHCI := y
CONFIG_DRIVER_PCI_NVME := n
CONFIG_DRIVER_USB_STORAGE := y
CONFIG_DRIVER_USB_CDC_NCM := n
CONFIG_DRIVER_USB_CDC_ECM := n
CONFIG_DRIVER_USB_HID := n
CONFIG_DRIVER_GRAPHICS := y
CONFIG_DRIVER_LGY98 := n
