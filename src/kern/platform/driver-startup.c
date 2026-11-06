/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Common driver startup.
 */

#include "kern/klog.h"

#if CONFIG_DRIVER_PCI
#include "drivers/pci/pci.h"
#endif

#if CONFIG_DRIVER_USB
#include "drivers/usb/usb.h"
#endif

#if CONFIG_DRIVER_PCI_UHCI
#include "drivers/pci/pci-uhci.h"
#endif

#if CONFIG_DRIVER_PCI_EHCI
#include "drivers/pci/pci-ehci.h"
#endif

#if CONFIG_DRIVER_PCI_XHCI
#include "drivers/pci/pci-xhci.h"
#endif

#if CONFIG_DRIVER_PCI_NVME
#include <drivers/pci/pci-nvme.h>
#include <drivers/pci/pci-vmd.h>
#endif

#if CONFIG_DRIVER_PCI_AHCI
#include <drivers/pci/pci-ahci.h>
#endif

#if CONFIG_DRIVER_PCI_I915
#include <drivers/pci/pci-i915.h>
#endif

#if CONFIG_DRIVER_PCI_NVRTX
#include <drivers/pci/pci-nvrtx.h>
#endif

#if CONFIG_DRIVER_PCI_AMDRDNA
#include <drivers/pci/pci-amdrdna.h>
#endif

#if CONFIG_DRIVER_PCI_GHCI
#include <drivers/pci/pci-ghci.h>
#endif

#if CONFIG_DRIVER_PCI_HDA
#include <drivers/pci/pci-hda.h>
#endif

#if CONFIG_DRIVER_USB_STORAGE
#include "drivers/usb/usb-storage.h"
#include <drivers/usb/usb-uas.h>
#endif

#if CONFIG_DRIVER_USB_CDC_NCM
#include <drivers/usb/usb-cdc-ncm.h>
#endif

#if CONFIG_DRIVER_USB_CDC_ECM
#include <drivers/usb/usb-cdc-ecm.h>
#endif

#if CONFIG_DRIVER_USB_RTL815X
#include <drivers/usb/usb-rtl815x.h>
#endif

#if CONFIG_DRIVER_USB_HID
#include <drivers/usb/usb-hid.h>
#include <drivers/usb/usb-hub.h>
#endif

#if CONFIG_DRIVER_USB_RTL8822BU
#include <drivers/usb/usb-rtl8822bu.h>
#endif

#if CONFIG_DRIVER_USB_CCID
#include <drivers/usb/usb-ccid.h>
#endif

#if CONFIG_DRIVER_USB_BT
#include <drivers/usb/usb-bt.h>
#endif

#if CONFIG_DRIVER_USB_UVC
#include <drivers/usb/usb-uvc.h>
#endif

#if CONFIG_DRIVER_PCI_INTEL_AX211
#include <drivers/wifi/intel-ax211/pci-intel-ax211.h>
#endif

#if CONFIG_DRIVER_PCI_RTL8125
#include <drivers/ethernet/rtl8125.h>
#endif

#if CONFIG_DRIVER_PCI_VENUS
#include <drivers/pci/pci-venus.h>
#endif

#if CONFIG_DRIVER_NE2000
#include "drivers/isa/pcat-ne2000.h"
#endif

/*
 * Registers the drivers for the PCI/PCIe devices.
 */
int
kern_driver_startup_pci_devices(
	void)
{
#if CONFIG_DRIVER_PCI
	if (drv_pci_init() != 0)
		kern_logf("pci: core initialization failed\n");
#endif

#if CONFIG_DRIVER_PCI_UHCI
	if (drv_pci_uhci_driver_register() != 0)
		kern_logf("usb: UHCI PCI driver registration failed\n");
#endif

#if CONFIG_DRIVER_PCI_EHCI
	if (drv_pci_ehci_driver_register() != 0)
		kern_logf("usb: EHCI PCI driver registration failed\n");
#endif

#if CONFIG_DRIVER_PCI_XHCI
	if (drv_pci_xhci_driver_register() != 0)
		kern_logf("usb: xHCI PCI driver registration failed\n");
#endif

#if CONFIG_DRIVER_PCI_NVME
	if (drv_pci_nvme_driver_register() != 0)
		kern_logf("nvme: PCI driver registration failed\n");
#endif

#if CONFIG_DRIVER_PCI_AHCI
	if (drv_pci_ahci_driver_register() != 0)
		kern_logf("ahci: PCI driver registration failed\n");
#endif

#if CONFIG_DRIVER_PCI_VMD
	if (drv_pci_vmd_driver_register() != 0)
		kern_logf("vmd: PCI driver registration failed\n");
#endif

#if CONFIG_DRIVER_PCI_VENUS
	if (drv_pci_venus_driver_register() != 0)
		kern_logf("pci: Venus driver registration failed\n");

#endif

#if CONFIG_DRIVER_PCI_I915
	if (drv_pci_i915_driver_register() != 0)
		kern_logf("pci: i915 driver registration failed\n");

#endif

#if CONFIG_DRIVER_PCI_NVRTX
	if (drv_pci_nvrtx_driver_register() != 0)
		kern_logf("pci: nvrtx driver registration failed\n");

#endif

#if CONFIG_DRIVER_PCI_AMDRDNA
	if (drv_pci_amdrdna_driver_register() != 0)
		kern_logf("pci: amdrdna driver registration failed\n");

#endif

#if CONFIG_DRIVER_PCI_GHCI
	if (drv_pci_ghci_driver_register() != 0)
		kern_logf("pci: GHCI driver registration failed\n");

#endif

#if CONFIG_DRIVER_PCI_HDA
	if (drv_pci_hda_driver_register() != 0)
		kern_logf("pci: HD Audio driver registration failed\n");

#endif

#if CONFIG_DRIVER_PCI_INTEL_AX211
	if (drv_pci_intel_ax211_driver_register() != 0)
		kern_logf("wlan: Intel AX211 PCI driver registration failed\n");
#endif

#if CONFIG_DRIVER_PCI_RTL8125
	if (drv_pci_rtl8125_driver_register() != 0)
		kern_logf("pci: RTL8125 driver registration failed\n");
#endif

#if CONFIG_DRIVER_PCI_LPSS_I2C
	if (drv_pci_lpss_i2c_driver_register() != 0)
		kern_logf("pci: LPSS I2C driver registration failed\n");

#endif

	return 0;
}

/*
 * Registers the drivers for the USB devices.
 */
int
kern_driver_startup_usb_devices(
	void)
{
#if CONFIG_DRIVER_USB
	if (drv_usb_init() != 0)
		kern_logf("usb: core initialization failed\n");
#endif

#if CONFIG_DRIVER_USB_STORAGE
	if (drv_usb_storage_driver_register() != 0)
		kern_logf("usb: mass-storage driver registration failed\n");
#endif

#if CONFIG_DRIVER_USB_UASP
	if (drv_usb_uas_driver_register() != 0)
		kern_logf("usb: UAS driver registration failed\n");
#endif

#if CONFIG_DRIVER_USB_CDC_NCM
	if (drv_usb_cdc_ncm_driver_register() != 0)
		kern_logf("usb: CDC NCM driver registration failed\n");
#endif

#if CONFIG_DRIVER_USB_CDC_ECM
	if (drv_usb_cdc_ecm_driver_register() != 0)
		kern_logf("usb: CDC ECM driver registration failed\n");
#endif

#if CONFIG_DRIVER_USB_RTL815X
	rtl815x_error = drv_usb_rtl815x_driver_register();
	if (rtl815x_error != 0)
		kern_logf("usb: Realtek RTL815x driver registration failed\n");
#endif

#if CONFIG_DRIVER_USB_RTL8822BU
	if (drv_usb_rtl8822bu_driver_register() != 0)
		kern_logf("usb: RTL8822BU WLAN driver registration failed\n");
#endif

#if CONFIG_KERNEL_USB_HID_CHECKPOINT
	if (drv_usb_hid_checkpoint_driver_register() != 0)
		kern_logf("usb: HID checkpoint driver registration failed\n");
#elif CONFIG_DRIVER_USB_HID
	if (drv_usb_hid_driver_register() != 0)
		kern_logf("usb: HID input driver registration failed\n");
#endif

#if CONFIG_DRIVER_USB_HUB
	if (drv_usb_hub_driver_register() != 0)
		kern_logf("usb: hub driver registration failed\n");
#endif

#if CONFIG_DRIVER_USB_CCID
	if (drv_usb_ccid_driver_register() != 0)
		kern_logf("usb: CCID smart card driver registration failed\n");
#endif

#if CONFIG_DRIVER_USB_BT
	if (drv_usb_bt_driver_register() != 0)
		kern_logf("usb: Bluetooth driver registration failed\n");
#endif

#if CONFIG_DRIVER_USB_UVC
	if (drv_usb_uvc_driver_register() != 0)
		kern_logf("usb: video class driver registration failed\n");
#endif

	return 0;
}

/*
 * Registers the drivers for the ISA devices.
 */
int
kern_driver_startup_isa_devices(
	void)
{
	/* Attaches the ISA NE2000 when one is configured and present. */
#if CONFIG_DRIVER_NE2000
	int err = drv_pcat_ne2000_init();
	if (err == 0)
		kern_logf("net: ISA NE2000 at 0x300 irq 10 registered as ne0\n");
	else if (err != ENODEV)
		kern_logf("net: ISA NE2000 initialization failed (%d)\n", err);
#endif

	return 0;
}

/*
 * Probes the USB roots and readies the PCI WLAN devices.
 */
void
kern_driver_probe_usb_roots_and_pci_wlan(
	void)
{
#if CONFIG_DRIVER_PCI_UHCI
	drv_pci_uhci_probe_roots();
#endif

#if CONFIG_DRIVER_PCI_EHCI
	drv_pci_ehci_probe_roots();
#endif

#if CONFIG_DRIVER_PCI_XHCI
	drv_pci_xhci_probe_roots();
#endif

#if CONFIG_DRIVER_PCI_INTEL_AX211
	drv_pci_intel_ax211_devices_ready();
#endif
}
