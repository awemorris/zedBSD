/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's ATA channels in the I/O controller.
 */

#ifndef KERN_DRIVERS_PLATFORM_MAC99_ATA_H
#define KERN_DRIVERS_PLATFORM_MAC99_ATA_H

struct disk;

/* A channel's boot drive word when the firmware did not boot from it. */
#define MAC99_ATA_NOT_BOOT	0xffffffffU

/*
 * Resets each ATA channel the firmware described and publishes each
 * drive that answers as an sd disk.
 */
int
drv_mac99_ata_init(void);

/*
 * Reports the disk of the drive the firmware booted from, or NULL.
 */
struct disk *
drv_mac99_ata_boot_disk(void);

#endif
