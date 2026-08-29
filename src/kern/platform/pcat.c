/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/platform.h"
#include "kern/disk.h"
#include "kern/clock.h"
#include "kern/sched.h"
#include "kern/partition.h"
#include <drivers/disklabel.h>
#include "drivers/pcat-ide.h"
#include "drivers/pci-pcat.h"
#include "drivers/hid/ps2-mouse.h"
#if CONFIG_DRIVER_PCI_UHCI
#include "drivers/pci-uhci.h"
#endif
#if CONFIG_DRIVER_PCI_EHCI
#include "drivers/pci-ehci.h"
#endif
#if CONFIG_DRIVER_PCI_XHCI
#include "drivers/pci-xhci.h"
#endif
#if CONFIG_DRIVER_USB_STORAGE
#include "drivers/usb-storage.h"
#endif
#if CONFIG_DRIVER_USB_CDC_NCM
#include <drivers/usb-cdc-ncm.h>
#endif
#include <drivers/pci.h>
#include <drivers/usb.h>
#if CONFIG_DRIVER_NE2000
#include "drivers/pcat-ne2000.h"
#endif
#if CONFIG_DRIVER_GRAPHICS_DEVICE
#include "drivers/graphics/pcat.h"
#endif
#include <errno.h>
#include <hal/hal.h>

size_t
kern_platform_init(const struct boot_handoff *handoff,
    struct boot_device *devices, size_t capacity)
{
	size_t count = 0;
	if (handoff == 0 || devices == 0 || capacity == 0 ||
	    handoff->magic != ZEDBSD_HANDOFF_MAGIC ||
	    handoff->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT) return 0;
	partition_set_scheme(&partition_scheme_mbr);
	disk_registry_reset();
	if (drv_pci_init() != 0)
		hal_printf("pci: core initialization failed\n");
#ifdef ZEDBSD_TEST_CHECKPOINTS
	{
		extern int ws004_pci_msi_qemu_register(void);
		if (ws004_pci_msi_qemu_register() != 0)
			hal_printf("WS004 MSI fixture registration failed\n");
	}
#endif
	if (drv_usb_init() != 0)
		hal_printf("usb: core initialization failed\n");
#if CONFIG_DRIVER_USB_STORAGE
	if (drv_usb_storage_driver_register() != 0)
		hal_printf("usb: mass-storage driver registration failed\n");
#endif
#if CONFIG_DRIVER_USB_CDC_NCM
	if (drv_usb_cdc_ncm_driver_register() != 0)
		hal_printf("usb: CDC NCM driver registration failed\n");
#endif
#if CONFIG_DRIVER_PCI_UHCI
	if (drv_pci_uhci_driver_register() != 0)
		hal_printf("usb: UHCI PCI driver registration failed\n");
#endif
#if CONFIG_DRIVER_PCI_EHCI
	if (drv_pci_ehci_driver_register() != 0)
		hal_printf("usb: EHCI PCI driver registration failed\n");
#endif
#if CONFIG_DRIVER_PCI_XHCI
	if (drv_pci_xhci_driver_register() != 0)
		hal_printf("usb: xHCI PCI driver registration failed\n");
#endif
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	if (pcat_graphics_pci_register() != 0)
		hal_printf("graphics: PCI driver registration failed\n");
#endif
	if (drv_pci_pcat_init() != 0)
		hal_printf("pci: PC/AT host initialization failed\n");
#ifdef ZEDBSD_TEST_CHECKPOINTS
	else
		drv_pci_dump();
#endif
	(void)pcat_ide_init();
	for (unsigned slot = 0; slot < 4U && count < capacity; slot++) {
		struct disk *disk = pcat_ide_bios_unit((uint8_t)(0x80U+slot));
		struct boot_device *device;
		if (disk == 0) continue;
		device = &devices[count];
		device->device_class=ZEDBSD_DEV_IDE; device->display_index=(uint8_t)count;
		device->bios_id=(uint8_t)(0x80U+slot); device->flags=ZEDBSD_DEV_PRESENT;
		if (device->bios_id == handoff->boot_bios_id)
			device->flags |= ZEDBSD_DEV_BOOT_ORIGIN;
		device->sector_size=512; device->cylinders=0; device->heads=0;
		device->sectors=0; device->controller_location=(uint8_t)slot;
		for (unsigned i=0;i<sizeof(device->reserved);i++) device->reserved[i]=0;
		count++;
	}
#if CONFIG_DRIVER_NE2000
	{
		int network_error = pcat_ne2000_init();

		if (network_error == 0)
			hal_printf("net: ISA NE2000 at 0x300 irq 10 registered "
			    "as ne0\n");
		else if (network_error != ENODEV)
			hal_printf("net: ISA NE2000 initialization failed (%d)\n",
			    network_error);
	}
#endif
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	if (!pcat_graphics_prepare())
		hal_printf("graphics: PC/AT driver unavailable\n");
#endif
	return count;
}

void kern_platform_refresh_devices(const struct boot_device *d, size_t n)
{
	uint64_t deadline;

	(void)d;
	(void)n;
#ifdef ZEDBSD_TEST_CHECKPOINTS
	{
		extern void ws004_pci_msi_qemu_raise(void);
		ws004_pci_msi_qemu_raise();
	}
#endif
#if CONFIG_DRIVER_PCI_UHCI
	drv_pci_uhci_probe_roots();
#endif
#if CONFIG_DRIVER_PCI_EHCI
	drv_pci_ehci_probe_roots();
#endif
#if CONFIG_DRIVER_PCI_XHCI
	drv_pci_xhci_probe_roots();
#endif
	if (disk_count() != 0)
		return;
	deadline = clock_ticks() + 5U * KERN_CLOCK_HZ;
	hal_printf("boot: waiting up to 5 seconds for boot storage\n");
	while (disk_count() == 0 && clock_ticks() < deadline)
		sched_yield();
	if (disk_count() == 0)
		hal_printf("boot: boot-storage wait expired\n");
}

int kern_platform_input_init(void) { return pcat_ps2_mouse_init(); }

struct disk *kern_platform_block_device(const struct boot_device *device)
{
	if (device == 0 || device->device_class != ZEDBSD_DEV_IDE) return 0;
	return pcat_ide_bios_unit(device->bios_id);
}

void kern_platform_debug_write(const char *text)
{
	while (text != 0 && *text != '\0') {
		uint8_t c = (uint8_t)*text++;
#ifdef HAL_PCAT_DEBUGCON
		__asm__ volatile("outb %0,$0xe9" : : "a"(c));
#else
		(void)c;
#endif
	}
}
void kern_platform_halt(void) { for (;;) __asm__ volatile("cli; hlt"); }
void kern_platform_reboot(void)
{
	for (unsigned spin=0;spin<1000000U;spin++) {
		uint8_t status; __asm__ volatile("inb $0x64,%0":"=a"(status));
		if (!(status&2U)) break;
	}
	__asm__ volatile("movb $0xfe,%%al; outb %%al,$0x64":::"eax");
	kern_platform_halt();
}
