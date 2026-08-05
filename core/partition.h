/*
 * Boots partition-table interface
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Partition-table interpretation is target dependent: PC-98 disks carry
 * a NEC-format table, PC/AT disks an MBR.  A platform registers one
 * scheme during initialisation and the rest of the loader scans through
 * this neutral interface.
 */

#ifndef BOOTS_PARTITION_H
#define BOOTS_PARTITION_H

#include "core/blkdev.h"

#define BOOTS_PARTITION_MAX 16U
#define BOOTS_PARTITION_NAME_MAX 17U

struct boots_partition {
	uint64_t start_lba;    /* Head of the partition (boot code side). */
	uint64_t data_lba;     /* Head of the data area (== start on MBR). */
	uint64_t sector_count; /* 0 when the format records no size (PC-98). */
	uint8_t bootable;
	char name[BOOTS_PARTITION_NAME_MAX];
};

struct boots_partition_scheme {
	const char *name;   /* "pc98" / "mbr" */
	/*
	 * Read the table from dev and fill entries in table order (so an
	 * empty slot yields a zeroed entry with bootable == 0 and an empty
	 * name).  Returns the number of table slots examined, or -1 on I/O
	 * failure.
	 */
	int (*scan)(const struct boots_partition_scheme *scheme,
		    struct boots_blkdev *dev,
		    struct boots_partition *entries, unsigned max_entries);
};

void boots_partition_set_scheme(const struct boots_partition_scheme *scheme);
const struct boots_partition_scheme *boots_partition_get_scheme(void);
int boots_partition_scan(struct boots_blkdev *dev,
			  struct boots_partition *entries,
			  unsigned max_entries);

#endif
