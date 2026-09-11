/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * NEC PC-98 internal IDE driver
 */

#ifndef KERN_DRIVERS_IDE_PC98_H
#define KERN_DRIVERS_IDE_PC98_H

#include "kern/disk.h"

struct boot_device;

/*
 * Probe both banks and register every present ATA disk with the block
 * device layer.  bios_devices supplies the firmware-sensed CHS geometry
 * (used for partition-table interpretation); units beyond the firmware
 * list fall back to their IDENTIFY geometry.  Returns the number of
 * disks registered.
 */
unsigned drv_pc98_ide_init(const struct boot_device *bios_devices,
			   unsigned bios_device_count);

/* The ordinal-th registered IDE disk, in probe (bank-major) order. */
struct disk *drv_pc98_ide_unit(unsigned ordinal);

/* Look up the physical IDE slot corresponding to BIOS unit 80h..83h. */
struct disk *drv_pc98_ide_bios_unit(uint8_t bios_id);

#endif
