/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 platform binding.
 *
 * The boot handoff carries the device table that the loader discovered;
 * the platform keeps the present disk and floppy entries, starts the
 * polled IDE driver, and optionally the LGY-98 network and graphics
 * drivers.
 */

#include "kern/platform.h"
#include "kern/disk.h"
#include "kern/partition.h"
#include <drivers/disklabel.h>
#include "drivers/platform/pc98/pc98-ide.h"
#include "drivers/hid/pc98-busmouse.h"
#if CONFIG_DRIVER_LGY98
#include "drivers/pc98-lgy98.h"
#endif
#if CONFIG_DRIVER_GRAPHICS_DEVICE
#include "drivers/graphics/pc98.h"
#endif
#include <errno.h>
#include <hal/hal.h>

/*
 * Publishes the present boot devices from the PC-98 handoff and starts
 * the platform drivers.
 *
 * Returns the number of devices published, which is zero for a handoff the
 * platform does not understand or one without a usable device.
 */
size_t
kern_platform_init(
	const struct boot_handoff *handoff,
	struct boot_device *devices,
	size_t capacity)
{
	const struct boot_device *initial;
	size_t count;
	size_t index;
#if CONFIG_DRIVER_LGY98
	int network_error;
#endif

	count = 0;

	/* Rejects a handoff that is not a PC-98 or Multiboot device table. */
	if (handoff == NULL ||
	    devices == NULL ||
	    capacity == 0 ||
	    handoff->magic != ZEDBSD_HANDOFF_MAGIC ||
	    (handoff->version != ZEDBSD_HANDOFF_VERSION_PC98 &&
	     handoff->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT) ||
	    handoff->size < sizeof(*handoff) ||
	    handoff->device_count == 0 ||
	    handoff->device_table == 0)
		return 0;

	/* A Multiboot handoff must name a primary MBR boot partition. */
	if (handoff->version == ZEDBSD_HANDOFF_VERSION_MULTIBOOT &&
	    (handoff->boot_partition_scheme != ZEDBSD_PARTITION_SCHEME_MBR ||
	     handoff->boot_partition_index < 1 ||
	     handoff->boot_partition_index > 4))
		return 0;

	/* Copies every present floppy, IDE, or SCSI entry that fits. */
	initial = (const struct boot_device *)handoff->device_table;
	for (index = 0; index < handoff->device_count && count < capacity; index++) {
		if ((initial[index].device_class != ZEDBSD_DEV_FDD &&
		     initial[index].device_class != ZEDBSD_DEV_IDE &&
		     initial[index].device_class != ZEDBSD_DEV_SCSI) ||
		    !(initial[index].flags & ZEDBSD_DEV_PRESENT))
			continue;
		devices[count++] = initial[index];
	}

	/* Reports failure without a single usable device. */
	if (count == 0)
		return 0;

	/* Selects the PC-98 partition scheme and starts the IDE driver. */
	partition_set_scheme(&drv_partition_scheme_pc98_auto);
	disk_registry_reset();
	(void)drv_pc98_ide_init(devices, (unsigned)count);

#if CONFIG_DRIVER_LGY98
	/* Starts the LGY-98 network interface when one is present. */
	network_error = drv_pc98_lgy98_init();
	if (network_error == 0) {
		hal_printf("net: LGY-98 registered as ne0\n");
		kern_platform_debug_write("net: LGY-98 registered as ne0\n");
	} else if (network_error != ENODEV) {
		hal_printf("net: LGY-98 initialization failed (%d)\n", network_error);
	}
#endif
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	/* Prepares the graphics driver, reporting its absence. */
	if (!drv_pc98_graphics_prepare())
		hal_printf("graphics: PC-98 driver unavailable\n");
#endif

	/* Reports the number of published devices. */
	return count;
}

/*
 * Refreshes the published devices after interrupts are enabled.
 *
 * The polled PC-98 IDE driver is fully initialized by
 * kern_platform_init().  Reinitializing it here would register the same
 * physical units a second time after interrupts are enabled.
 */
void
kern_platform_refresh_devices(
	const struct boot_device *devices,
	size_t count)
{
	(void)devices;
	(void)count;
}

/*
 * Initializes the PC-98 bus mouse.
 */
int
kern_platform_input_init(
	void)
{
	int error;

	/* Starts the bus mouse driver. */
	error = drv_pc98_busmouse_init();

	/* Reports the driver result. */
	return error;
}

/*
 * Resolves a published boot device to its disk.
 */
struct disk *
kern_platform_block_device(
	const struct boot_device *device)
{
	struct disk *disk;

	/* Only an IDE boot device has a disk. */
	if (device == NULL || device->device_class != ZEDBSD_DEV_IDE)
		return NULL;

	/* Resolves the IDE unit by its BIOS identifier. */
	disk = drv_pc98_ide_bios_unit(device->bios_id);

	/* Reports the disk. */
	return disk;
}

/*
 * Writes a debug string to the emulator debug port.
 */
void
kern_platform_debug_write(
	const char *text)
{
	uint8_t character;

	/* Writes each byte to port 0xe9. */
	while (*text) {
		character = (uint8_t)*text++;

		asm volatile("outb %0,$0xe9" : : "a"(character));
	}
}

/*
 * Halts the machine permanently.
 */
void
kern_platform_halt(
	void)
{
	/* Halts with interrupts disabled. */
	for (;;)
		asm volatile("cli; hlt");
}

/*
 * Reboots the machine through the keyboard controller.
 */
void
kern_platform_reboot(
	void)
{
	/* Requests the reset and spins in case it is slow to take effect. */
	asm volatile("movb $0x0f,%%al; outb %%al,$0x37" ::: "eax");
	for (;;)
		;
}
