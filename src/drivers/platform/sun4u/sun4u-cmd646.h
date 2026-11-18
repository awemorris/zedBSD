/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CMD646 IDE controller's primary channel, driven by programmed I/O.
 */

#ifndef KERN_DRIVERS_SUN4U_CMD646_H
#define KERN_DRIVERS_SUN4U_CMD646_H

#include <stdint.h>

struct disk;

/*
 * Resets the channel, identifies its first drive and publishes it as a
 * disk.
 */
int
drv_sun4u_cmd646_init(
	uint16_t command_port,
	uint16_t control_port);

/*
 * Reports the published disk, or NULL when none was found.
 */
struct disk *
drv_sun4u_cmd646_disk(
	void);

#endif
