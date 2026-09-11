/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * CMD646 primary-channel ATA PIO driver for QEMU sun4u.
 */

#ifndef KERN_DRIVERS_SUN4U_CMD646_H
#define KERN_DRIVERS_SUN4U_CMD646_H

#include <stdint.h>

struct disk;

int drv_sun4u_cmd646_init(uint16_t command_port, uint16_t control_port);

struct disk *drv_sun4u_cmd646_disk(void);

#endif
