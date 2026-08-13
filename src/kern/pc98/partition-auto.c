/* Per-disk PC-98 partition format selection.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/mbr-partition.h"
#include "kern/pc98/partition.h"
#include "kern/pc98/partition-auto.h"

static int
pc98_auto_scan(const struct partition_scheme *scheme, struct disk *disk,
    struct partition *entries, unsigned capacity)
{
	uint8_t sector[512];

	(void)scheme;
	if (disk == NULL || disk->d_block_size != 512U ||
	    disk_read(disk, 0, 1, sector) != 0)
		return -1;
	if (sector[510] == 0x55U && sector[511] == 0xaaU)
		return partition_scheme_mbr.scan(&partition_scheme_mbr, disk,
		    entries, capacity);
	return partition_scheme_pc98.scan(&partition_scheme_pc98, disk,
	    entries, capacity);
}

const struct partition_scheme partition_scheme_pc98_auto = {
	.name = "pc98-auto",
	.scan = pc98_auto_scan,
};

