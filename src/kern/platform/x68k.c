/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000 native platform binding.
 *
 * The boot handoff identifies the SCSI bus; every attached SPC disk is
 * published as a boot device, with the unit the IPL booted from marked as
 * the boot origin.
 */

#include <errno.h>
#include <hal/hal.h>
#include <drivers/disklabel.h>
#include <kern/disk.h>
#include <kern/platform.h>
#include <kern/partition.h>
#include "drivers/platform/x68k/x68k-spc-disk.h"
#include "hal/m68k/bsp-x68k/bsp.h"
#include "hal/m68k/bsp-x68k/scsi.h"

/*
 * Publishes the SCSI disks found on the X68000 bus as boot devices.
 *
 * Returns the number of devices published, which is zero for a handoff the
 * platform does not understand or a bus without disks.
 */
size_t
kern_platform_init(
	const struct boot_handoff *common,
	struct boot_device *devices,
	size_t capacity)
{
	const struct x68k_boot_handoff *handoff;
	struct x68k_spc_bus bus;
	struct boot_device *device;
	unsigned initiator;
	unsigned target;
	size_t count;

	handoff = (const void *)common;
	count = 0;

	/* Rejects a missing device table or an invalid handoff. */
	if (devices == NULL ||
	    capacity == 0 ||
	    !x68k_boot_handoff_valid(handoff))
		return 0;

	/* Selects the X68000 partition scheme and probes the SCSI bus. */
	partition_set_scheme(&drv_partition_scheme_x68k);
	disk_registry_reset();
	x68k_bsp_spc_bus(&bus);
	initiator = x68k_bsp_scsi_initiator_id();
	if (drv_x68k_spc_disk_init(&bus, initiator, handoff->common.boot_bios_id) == 0)
		return 0;

	/* Publishes every attached target that fits the table. */
	for (target = 0; target < 7U && count < capacity; target++) {
		if (drv_x68k_spc_disk_target(target) == NULL)
			continue;

		/* Describes one present SCSI target for the boot record. */
		device = &devices[count];
		hal_memset(device, 0, sizeof(*device));
		device->device_class = KERN_DEV_SCSI;
		device->display_index = (uint8_t)count;
		device->bios_id = (uint8_t)target;
		device->flags = KERN_DEV_PRESENT;
		if (target == handoff->common.boot_bios_id)
			device->flags |= KERN_DEV_BOOT_ORIGIN;
		device->sector_size = X68K_SCSI_BLOCK_SIZE;
		device->controller_location = (uint8_t)target;
		count++;
	}

	/* Reports the number of published devices. */
	return count;
}

/*
 * Refreshes the published devices; the X68000 has no hot-pluggable devices.
 */
void
kern_platform_refresh_devices(
	const struct boot_device *d,
	size_t n)
{
	(void)d;
	(void)n;
}

/*
 * Initializes platform input devices; the X68000 has none here.
 */
int
kern_platform_input_init(
	void)
{
	/* Reports success without any input device. */
	return 0;
}

/*
 * Resolves a published boot device to its disk.
 */
struct disk *
kern_platform_block_device(
	const struct boot_device *device)
{
	struct disk *disk;

	/* Only a SCSI boot device has a disk. */
	if (device == NULL || device->device_class != KERN_DEV_SCSI)
		return NULL;

	/* Resolves the SPC target that the device names. */
	disk = drv_x68k_spc_disk_target(device->bios_id);

	/* Reports the disk. */
	return disk;
}

/*
 * Writes a debug string to the console.
 */
void
kern_platform_debug_write(
	const char *text)
{
	/* Ignores a missing string. */
	if (text != NULL)
		hal_cons_write(text);
}

/*
 * Halts the machine permanently.
 */
void
kern_platform_halt(
	void)
{
	/* Halts with interrupts disabled. */
	(void)hal_irq_disable();
	for (;;)
		hal_halt();
}

/*
 * Reboots the machine.
 */
void
kern_platform_reboot(
	void)
{
	/* Halts in case the reset request returns. */
	hal_reset();
	for (;;)
		hal_halt();
}
