/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/platform.h"
#include "kern/disk.h"
#include "kern/partition.h"
#include <drivers/disklabel.h>
#include "drivers/pc98-ide.h"
#include "drivers/pc98-busmouse.h"
#if CONFIG_DRIVER_LGY98
#include "drivers/pc98-lgy98.h"
#endif
#if CONFIG_DRIVER_GRAPHICS_DEVICE
#include "drivers/graphics/pc98.h"
#endif
#include <errno.h>
#include <hal/hal.h>

size_t
kern_platform_init(const struct boot_handoff *handoff,
		   struct boot_device *devices, size_t capacity)
{
	const struct boot_device *initial;
	size_t count = 0;

	if (handoff == NULL || devices == NULL || capacity == 0 ||
	    handoff->magic != ZEDBSD_HANDOFF_MAGIC ||
	    (handoff->version != ZEDBSD_HANDOFF_VERSION_PC98 &&
	     handoff->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT) ||
	    handoff->size < sizeof(*handoff) || handoff->device_count == 0 ||
	    handoff->device_table == 0)
		return 0;
	if (handoff->version == ZEDBSD_HANDOFF_VERSION_MULTIBOOT &&
	    (handoff->boot_partition_scheme != ZEDBSD_PARTITION_SCHEME_MBR ||
	     handoff->boot_partition_index < 1 ||
	     handoff->boot_partition_index > 4))
		return 0;
	initial = (const struct boot_device *)handoff->device_table;
	for (size_t index = 0; index < handoff->device_count && count < capacity;
	     index++) {
		if ((initial[index].device_class != ZEDBSD_DEV_FDD &&
		     initial[index].device_class != ZEDBSD_DEV_IDE &&
		     initial[index].device_class != ZEDBSD_DEV_SCSI) ||
		    !(initial[index].flags & ZEDBSD_DEV_PRESENT))
			continue;
		devices[count++] = initial[index];
	}
	if (count == 0)
		return 0;

	partition_set_scheme(&partition_scheme_pc98_auto);
	disk_registry_reset();
	(void)pc98_ide_init(devices, (unsigned)count);
#if CONFIG_DRIVER_LGY98
	{
		int network_error = pc98_lgy98_init();

		if (network_error == 0)
		{
			hal_printf("net: LGY-98 registered as ne0\n");
			kern_platform_debug_write("net: LGY-98 registered as ne0\n");
		}
		else if (network_error != ENODEV)
			hal_printf("net: LGY-98 initialization failed (%d)\n",
			    network_error);
	}
#endif
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	if (!pc98_graphics_prepare())
		hal_printf("graphics: PC-98 driver unavailable\n");
#endif
	return count;
}

void
kern_platform_refresh_devices(const struct boot_device *devices, size_t count)
{
	/*
	 * The polled PC-98 IDE driver is fully initialized by
	 * kern_platform_init().  Reinitializing it here would register the same
	 * physical units a second time after interrupts are enabled.
	 */
	(void)devices;
	(void)count;
}

int
kern_platform_input_init(void)
{
	return pc98_busmouse_init();
}

struct disk *
kern_platform_block_device(const struct boot_device *device)
{
	if (device == NULL || device->device_class != ZEDBSD_DEV_IDE)
		return NULL;
	return pc98_ide_bios_unit(device->bios_id);
}

void
kern_platform_debug_write(const char *text)
{
	while (*text) {
		uint8_t character = (uint8_t)*text++;

		asm volatile("outb %0,$0xe9" : : "a"(character));
	}
}

void
kern_platform_halt(void)
{
	for (;;)
		asm volatile("cli; hlt");
}

void
kern_platform_reboot(void)
{
	asm volatile("movb $0x0f,%%al; outb %%al,$0x37" ::: "eax");
	for (;;)
		;
}
