/*
 * PC/AT ATA PIO driver
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_DRIVERS_PCAT_IDE_H
#define ZEDBSD_DRIVERS_PCAT_IDE_H
#include "kern/disk.h"

unsigned zedbsd_ide_pcat_init(void);
struct disk *zedbsd_ide_pcat_unit(unsigned ordinal);
struct disk *zedbsd_ide_pcat_bios_unit(uint8_t bios_id);

#endif
