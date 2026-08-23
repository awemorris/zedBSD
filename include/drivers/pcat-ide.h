/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC/AT ATA PIO driver
 */
#ifndef ZEDBSD_DRIVERS_PCAT_IDE_H
#define ZEDBSD_DRIVERS_PCAT_IDE_H
#include "kern/disk.h"

unsigned
pcat_ide_init(void);
struct disk *
pcat_ide_unit(
	unsigned ordinal);
struct disk *
pcat_ide_bios_unit(
	uint8_t bios_id);

#endif
