/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * X68000 MB89352 synchronous polled-PIO block driver.
 */

#ifndef KERN_DRIVERS_X68K_SPC_DISK_H
#define KERN_DRIVERS_X68K_SPC_DISK_H

#include "drivers/platform/x68k/x68k-mb89352.h"

struct disk;

unsigned drv_x68k_spc_disk_init(const struct x68k_spc_bus *bus,
				unsigned initiator_id, unsigned boot_target_id);
struct disk *drv_x68k_spc_disk_target(unsigned target_id);

#endif
