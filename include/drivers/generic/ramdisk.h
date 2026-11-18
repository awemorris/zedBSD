/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A block device whose blocks are a range of memory.
 *
 * The firmware or the boot loader leaves a disk image in RAM (an initial
 * ramdisk), and the platform publishes it as a disk the partition and file
 * system layers read like any other.
 */

#ifndef KERN_DRIVERS_GENERIC_RAMDISK_H
#define KERN_DRIVERS_GENERIC_RAMDISK_H

#include <stdint.h>

struct disk;

/*
 * Publishes the memory at base, size bytes long, as the disk named name.
 *
 * The memory must stay mapped and untouched by anything else for the life
 * of the system; the disk reads and writes it in place.  The size must be a
 * whole number of 512-byte blocks.  Returns 0 and the disk, or an errno.
 */
int drv_ramdisk_create(const char *name, void *base, uint64_t size, struct disk **disk_out);

#endif
