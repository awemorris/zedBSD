/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The emulators' RISC-V virt board: its PCI host and what sits behind it.
 *
 * The board has one generic ECAM host whose legacy pins arrive on the
 * PLIC.  The drivers the configuration selects are registered before the
 * bus is enumerated, so enumeration binds them.  A board without the host,
 * or a host that cannot be started, leaves the machine without PCI devices
 * but does not stop the boot.
 */

#include "drivers/platform/riscv-virt/virt-pci.h"
#include <drivers/pci/pci.h>
#include <drivers/pci/pci-ecam-fdt.h>
#include <drivers/pci/pci-nvme.h>
#include <drivers/pci/pci-xhci.h>
#include <drivers/usb/usb.h>
#include <drivers/usb/usb-hid.h>
#include <drivers/usb/usb-uvc.h>
#include <drivers/usb/usb-hub.h>
#include <drivers/usb/usb-storage.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

static void register_drivers(void);

/*
 * Starts the PCI core and the board's ECAM host.
 */
int
drv_riscv_virt_pci_init(
	const struct drv_fdt *fdt)
{
	struct drv_pci_ecam_fdt_config *config;
	struct drv_pci_ecam_fdt *host;
	uint32_t node;
	int error;

	/* Brings the PCI core into service, even when no host follows. */
	error = drv_pci_init();
	if (error != 0 && error != EALREADY)
		return error;

	/* Registers the drivers, so enumeration binds the devices. */
	register_drivers();

	/* Finds the host; a board without one has nothing to do. */
	if (fdt == NULL)
		return ENODEV;
	error = drv_fdt_find_compatible(fdt, "pci-host-ecam-generic", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENODEV;

	/* Allocates the host's description, which is too large for the stack. */
	config = kern_malloc(sizeof(*config));
	if (config == NULL)
		return ENOMEM;

	/* Reads the host's windows and interrupt map. */
	error = drv_pci_ecam_fdt_describe(fdt, node, config);
	if (error != 0) {
		kern_logf("pci: unusable ECAM host node (%d)\n", error);
		kern_free(config);
		return error;
	}

	/* Maps the configuration space and places the BARs. */
	error = drv_pci_ecam_fdt_start(config, &host);
	kern_free(config);
	if (error != 0) {
		kern_logf("pci: ECAM host start failed (%d)\n", error);
		return error;
	}

	/* Enumerates the bus, which binds the registered drivers. */
	error = drv_pci_ecam_fdt_publish(host);
	if (error != 0) {
		kern_logf("pci: ECAM enumeration failed (%d)\n", error);
		return error;
	}

	/* Succeeded: the PCI core owns the host's tree. */
	return 0;
}

/*
 * Finishes discovery behind the host once interrupts are enabled.
 */
void
drv_riscv_virt_pci_refresh(
	void)
{
	/* Has each xHCI controller look at its root ports. */
#if CONFIG_DRIVER_PCI_XHCI
	drv_pci_xhci_probe_roots();
#endif

	/* Has each NVMe controller publish its namespace. */
#if CONFIG_DRIVER_PCI_NVME
	drv_pci_nvme_probe_namespaces();
#endif
}

/*
 * Lets the USB keyboards and mice type once the input core is ready.
 */
void
drv_riscv_virt_pci_input_ready(
	void)
{
	/* Starts the HID devices found so far and those found later. */
#if CONFIG_DRIVER_USB_HID
	drv_usb_hid_input_ready();
#endif
}

/*
 * Registers the drivers the configuration selects.
 *
 * A failure is recorded; the boot goes on without that driver.
 */
static void
register_drivers(
	void)
{
#if CONFIG_DRIVER_PCI_XHCI || CONFIG_DRIVER_PCI_NVME
	int error;
#endif

#if CONFIG_DRIVER_PCI_XHCI
	/* Brings up the USB core. */
	error = drv_usb_init();
	if (error != 0) {
		kern_logf("usb: core initialization failed (%d)\n", error);
	} else {
		/* Registers bulk-only USB disks. */
#if CONFIG_DRIVER_USB_STORAGE
		error = drv_usb_storage_driver_register();
		if (error != 0)
			kern_logf("usb: storage registration failed (%d)\n", error);
#endif

		/* Registers the USB Video Class cameras. */
#if CONFIG_DRIVER_USB_UVC
		error = drv_usb_uvc_driver_register();
		if (error != 0)
			kern_logf("usb: video class registration failed (%d)\n", error);
#endif

		/* Registers the keyboard and mouse driver. */
#if CONFIG_DRIVER_USB_HID
		error = drv_usb_hid_driver_register();
		if (error != 0)
			kern_logf("usb: HID input driver registration failed (%d)\n", error);
#endif

		/* Registers the hub driver. */
#if CONFIG_DRIVER_USB_HUB
		error = drv_usb_hub_driver_register();
		if (error != 0)
			kern_logf("usb: hub driver registration failed (%d)\n", error);
#endif

		/* Registers the host controller driver. */
		error = drv_pci_xhci_driver_register();
		if (error != 0)
			kern_logf("usb: xHCI PCI driver registration failed (%d)\n", error);
	}
#endif

	/* Registers the NVMe controller driver. */
#if CONFIG_DRIVER_PCI_NVME
	error = drv_pci_nvme_driver_register();
	if (error != 0)
		kern_logf("nvme: PCI driver registration failed (%d)\n", error);
#endif
}
