/*
 * Boots block device interface
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * A thin registry of block devices.  Drivers live under drivers/ and
 * register through this interface; the filesystem layer and the boot
 * loader consume it without knowing the underlying transport (IDE now,
 * SCSI later).  There is no dynamic allocation: the registry is a fixed
 * table populated during platform initialisation.
 */

#ifndef BOOTS_BLKDEV_H
#define BOOTS_BLKDEV_H

#include <stddef.h>
#include <stdint.h>

#define BOOTS_BLKDEV_MAX 8U
#define BOOTS_BLKDEV_NAME_MAX 8U

enum boots_blkdev_result {
	BOOTS_BLKDEV_OK = 0,
	BOOTS_BLKDEV_IO_ERROR,
	BOOTS_BLKDEV_OUT_OF_RANGE,
	BOOTS_BLKDEV_READ_ONLY,
	BOOTS_BLKDEV_INVALID,
};

struct boots_blkdev {
	char name[BOOTS_BLKDEV_NAME_MAX];   /* "ide0".. NUL-terminated */
	uint16_t sector_size;               /* 512 for the supported disks */
	uint64_t sector_count;
	/*
	 * CHS geometry used only to interpret partition tables.  Partition
	 * tables are recorded in the geometry the firmware used, which need
	 * not match the drive's native IDENTIFY geometry, so drivers fill
	 * these from the firmware-sensed values.  Zero means no geometry.
	 */
	uint16_t heads;
	uint16_t sectors_per_track;
	enum boots_blkdev_result (*read)(struct boots_blkdev *dev,
					  uint64_t lba, uint32_t count,
					  void *buffer);
	/* NULL for a read-only device. */
	enum boots_blkdev_result (*write)(struct boots_blkdev *dev,
					   uint64_t lba, uint32_t count,
					   const void *buffer);
	enum boots_blkdev_result (*flush)(struct boots_blkdev *dev);
	void *private_data;
};

/* Register a device.  Returns 1 on success, 0 if the table is full or
 * the descriptor is malformed.  The descriptor must stay live (drivers
 * use static storage). */
int boots_blkdev_register(struct boots_blkdev *dev);

/* Forget every registered device (used by tests and re-probe). */
void boots_blkdev_reset(void);

unsigned boots_blkdev_count(void);
struct boots_blkdev *boots_blkdev_get(unsigned index);
struct boots_blkdev *boots_blkdev_find(const char *name);

/* Convenience wrappers with range checking. */
enum boots_blkdev_result boots_blkdev_read(struct boots_blkdev *dev,
					    uint64_t lba, uint32_t count,
					    void *buffer);
enum boots_blkdev_result boots_blkdev_write(struct boots_blkdev *dev,
					     uint64_t lba, uint32_t count,
					     const void *buffer);

#endif
