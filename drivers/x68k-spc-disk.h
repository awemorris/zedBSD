/* X68000 MB89352 polled-PIO disk adapter. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_DRIVERS_X68K_SPC_DISK_H
#define ZEDBSD_DRIVERS_X68K_SPC_DISK_H

#include "drivers/x68k-mb89352.h"

struct disk;

unsigned x68k_spc_disk_init(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned boot_target_id);
struct disk *x68k_spc_disk_target(unsigned target_id);

#endif
