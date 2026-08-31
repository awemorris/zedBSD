# Focused ws004-p031 amd64/UEFI legacy-HCD runtime selection.
#
# Keep this configuration independent of the developer's config.mk.  The QEMU
# lifecycle runner attaches exactly one legacy controller per boot, while both
# drivers remain linked so the same image is used for the UHCI and EHCI cells.
ZEDBSD_MENU_VERSION := 3
ZEDBSD_PLATFORM := amd64
ZEDBSD_ARCHITECTURE := amd64
ZEDBSD_BOARD := pcat
ZEDBSD_VARIANT := uefi

CONFIG_KERNEL_TEST_CHECKPOINTS := y
CONFIG_KERNEL_USB_HID_CHECKPOINT := y
CONFIG_BUF_CACHE_KIB := 0
CONFIG_DRIVER_NE2000 := n
CONFIG_DRIVER_PCI_UHCI := y
CONFIG_DRIVER_PCI_EHCI := y
CONFIG_DRIVER_PCI_XHCI := n
CONFIG_DRIVER_PCI_NVME := n
CONFIG_DRIVER_USB_STORAGE := y
CONFIG_DRIVER_USB_CDC_NCM := n
CONFIG_DRIVER_GRAPHICS := y
CONFIG_DRIVER_LGY98 := n

# init/login/reboot and the other mandatory base programs are added by the
# top-level Makefile.  dd supplies the bounded bulk-I/O request used by the
# phase-owned hot-unplug cell; cmp verifies that the independent storage path
# still returns the same payload after the HID hotplug cycles.  sleep provides
# the explicit guest-side overlap/reboot staging interval.
ZEDBSD_USER_PROGRAMS := dd cmp sleep
