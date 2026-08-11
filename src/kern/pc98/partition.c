/*
 * NEC PC-98 partition-table scheme
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * The table lives in the sector after the IPL (LBA 1 on the 512-byte
 * disks this loader supports) and holds sixteen 32-byte entries.  All
 * addresses are CHS in the geometry the firmware sensed at boot, which
 * is why struct boots_blkdev carries that geometry rather than the
 * drive's native IDENTIFY values.  The format records no partition
 * size, so sector_count stays 0.
 */

#include "kern/partition.h"

#define PC98_TABLE_LBA 1U
#define PC98_ENTRY_SIZE 32U

static uint16_t
get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* sect, head, cyl(16bit) -> LBA in the firmware geometry. */
static uint64_t
chs_to_lba(const struct boots_blkdev *dev, const uint8_t *p)
{
	return ((uint64_t)get16(p + 2) * dev->heads + p[1]) *
		dev->sectors_per_track + p[0];
}

static int
pc98_scan(const struct boots_partition_scheme *scheme,
	  struct boots_blkdev *dev, struct boots_partition *entries,
	  unsigned max_entries)
{
	uint8_t sector[512];
	unsigned count;
	unsigned i;

	(void)scheme;
	if (dev->sector_size != 512 || dev->heads == 0 ||
	    dev->sectors_per_track == 0)
		return -1;
	if (boots_blkdev_read(dev, PC98_TABLE_LBA, 1, sector) !=
	    BOOTS_BLKDEV_OK)
		return -1;
	count = 512U / PC98_ENTRY_SIZE;
	if (count > max_entries)
		count = max_entries;
	for (i = 0; i < count; i++) {
		const uint8_t *p = sector + i * PC98_ENTRY_SIZE;
		struct boots_partition *entry = &entries[i];
		unsigned j;

		entry->start_lba = 0;
		entry->data_lba = 0;
		entry->sector_count = 0;
		entry->bootable = 0;
		entry->name[0] = '\0';
		if (p[0] == 0)
			continue;
		entry->bootable = (p[0] & 0x80U) && (p[1] & 0x80U);
		entry->start_lba = chs_to_lba(dev, p + 4);
		entry->data_lba = chs_to_lba(dev, p + 8);
		for (j = 0; j < 16; j++) {
			char c = (char)p[16 + j];

			if (c == '\0' || c == ' ')
				break;
			entry->name[j] = c;
		}
		entry->name[j] = '\0';
	}
	return (int)count;
}

const struct boots_partition_scheme boots_partition_scheme_pc98 = {
	.name = "pc98",
	.scan = pc98_scan,
};
