/*
 * NEC PC-98 internal IDE driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_DRIVERS_IDE_PC98_H
#define BOOTS_DRIVERS_IDE_PC98_H

#include "kern/disk.h"

struct boots_device;

/*
 * Probe both banks and register every present ATA disk with the block
 * device layer.  bios_devices supplies the firmware-sensed CHS geometry
 * (used for partition-table interpretation); units beyond the firmware
 * list fall back to their IDENTIFY geometry.  Returns the number of
 * disks registered.
 */
unsigned boots_ide_pc98_init(const struct boots_device *bios_devices,
			      unsigned bios_device_count);

/* The ordinal-th registered IDE disk, in probe (bank-major) order. */
struct disk *boots_ide_pc98_unit(unsigned ordinal);

/* Look up the physical IDE slot corresponding to BIOS unit 80h..83h. */
struct disk *boots_ide_pc98_bios_unit(uint8_t bios_id);

#endif
