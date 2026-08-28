/* X68000 native platform device binding. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <errno.h>
#include <hal/hal.h>
#include <drivers/disklabel.h>
#include <kern/disk.h>
#include <kern/platform.h>
#include <kern/partition.h>
#include "drivers/x68k-spc-disk.h"
#include "hal/m68k/bsp-x68k/bsp.h"
#include "hal/m68k/bsp-x68k/scsi.h"

size_t
kern_platform_init(const struct boot_handoff *common,
	struct boot_device *devices, size_t capacity)
{
	const struct x68k_boot_handoff *handoff = (const void *)common;
	struct x68k_spc_bus bus;
	unsigned initiator, target;
	size_t count = 0;
	if (devices == NULL || capacity == 0 ||
	    !x68k_boot_handoff_valid(handoff))
		return 0;
	partition_set_scheme(&partition_scheme_x68k);
	disk_registry_reset();
	x68k_bsp_spc_bus(&bus);
	initiator = x68k_bsp_scsi_initiator_id();
	if (x68k_spc_disk_init(&bus, initiator,
	    handoff->common.boot_bios_id) == 0)
		return 0;
	for (target = 0; target < 7U && count < capacity; target++) {
		struct boot_device *device;
		if (x68k_spc_disk_target(target) == NULL)
			continue;
		device = &devices[count];
		hal_memset(device, 0, sizeof(*device));
		device->device_class = ZEDBSD_DEV_SCSI;
		device->display_index = (uint8_t)count;
		device->bios_id = (uint8_t)target;
		device->flags = ZEDBSD_DEV_PRESENT;
		if (target == handoff->common.boot_bios_id)
			device->flags |= ZEDBSD_DEV_BOOT_ORIGIN;
		device->sector_size = X68K_SCSI_BLOCK_SIZE;
		device->controller_location = (uint8_t)target;
		count++;
	}
	return count;
}

void kern_platform_refresh_devices(const struct boot_device *d, size_t n)
{ (void)d; (void)n; }

int kern_platform_input_init(void) { return 0; }

struct disk *
kern_platform_block_device(const struct boot_device *device)
{
	return device != NULL && device->device_class == ZEDBSD_DEV_SCSI ?
	    x68k_spc_disk_target(device->bios_id) : NULL;
}

void kern_platform_debug_write(const char *text)
{ if (text != NULL) hal_cons_write(text); }
void kern_platform_halt(void)
{ (void)hal_irq_disable(); for (;;) hal_halt(); }
void kern_platform_reboot(void)
{ hal_reset(); for (;;) hal_halt(); }
