/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/platform.h"
#include "kern/disk.h"
#include "kern/partition.h"
#include "kern/mbr-partition.h"
#include "drivers/pcat-ide.h"
#include "drivers/pci-pcat.h"
#include <drivers/pci.h>
#if CONFIG_DRIVER_NE2000
#include "drivers/pcat-ne2000.h"
#endif
#if CONFIG_DRIVER_GRAPHICS
#include "drivers/pcat-graphics.h"
#endif
#include <errno.h>
#include <hal/hal.h>

size_t
kern_platform_init(const struct zedbsd_handoff *handoff,
    struct zedbsd_device *devices, size_t capacity)
{
	size_t count = 0;
	if (handoff == 0 || devices == 0 || capacity == 0 ||
	    handoff->magic != ZEDBSD_HANDOFF_MAGIC ||
	    handoff->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT) return 0;
	partition_set_scheme(&partition_scheme_mbr);
	disk_registry_reset();
	if (drv_pci_init() != 0)
		hal_printf("pci: core initialization failed\n");
#if CONFIG_DRIVER_GRAPHICS
	else if (zedbsd_pcat_graphics_driver_register() != 0)
		hal_printf("graphics: PCI driver registration failed\n");
#endif
	if (drv_pci_pcat_init() != 0)
		hal_printf("pci: PC/AT host initialization failed\n");
	(void)zedbsd_ide_pcat_init();
	for (unsigned slot = 0; slot < 4U && count < capacity; slot++) {
		struct disk *disk = zedbsd_ide_pcat_bios_unit((uint8_t)(0x80U+slot));
		struct zedbsd_device *device;
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
		int network_error = zedbsd_pcat_ne2000_init();

		if (network_error == 0)
			hal_printf("net: ISA NE2000 at 0x300 irq 10 registered "
			    "as ne0\n");
		else if (network_error != ENODEV)
			hal_printf("net: ISA NE2000 initialization failed (%d)\n",
			    network_error);
	}
#endif
#if CONFIG_DRIVER_GRAPHICS
	if (!zedbsd_pcat_graphics_init())
		hal_printf("graphics: PC/AT driver unavailable\n");
#endif
	return count;
}

void kern_platform_refresh_devices(const struct zedbsd_device *d, size_t n)
{ (void)d; (void)n; }

struct disk *kern_platform_block_device(const struct zedbsd_device *device)
{
	if (device == 0 || device->device_class != ZEDBSD_DEV_IDE) return 0;
	return zedbsd_ide_pcat_bios_unit(device->bios_id);
}

void kern_platform_debug_write(const char *text)
{
	while (text != 0 && *text != '\0') {
		uint8_t c=(uint8_t)*text++; hal_cons_putc(c);
#ifdef HAL_PCAT_DEBUGCON
		__asm__ volatile("outb %0,$0xe9" : : "a"(c));
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
