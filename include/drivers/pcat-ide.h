/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC/AT ATA PIO driver
 */
#ifndef KERN_DRIVERS_PCAT_IDE_H
#define KERN_DRIVERS_PCAT_IDE_H
#include "kern/disk.h"

unsigned
drv_pcat_ide_init(void);
struct disk *
drv_pcat_ide_unit(
	unsigned ordinal);
struct disk *
drv_pcat_ide_bios_unit(
	uint8_t bios_id);

#endif
