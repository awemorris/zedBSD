/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Per-disk PC-98 partition format selection.
 */

#include <drivers/disklabel.h>

#define PC98_TABLE_LBA 1U
#define PC98_ENTRY_SIZE 32U

static int pc98_auto_scan(const struct partition_scheme *scheme, struct disk *disk, struct partition *entries, unsigned capacity);
static int pc98_scan(const struct partition_scheme *scheme, struct disk *dev, struct partition *entries, unsigned max_entries);
static int chs_to_lba(const struct disk_geometry *geometry, const uint8_t *p, uint64_t *result);
static uint16_t get16(const uint8_t *p);

const struct partition_scheme drv_partition_scheme_pc98_auto = {
	.name = "pc98-auto",
	.scan = pc98_auto_scan,
};

const struct partition_scheme drv_partition_scheme_pc98 = {
	.name = "pc98",
	.scan = pc98_scan,
};

/* Supports the pc98 auto scan operation. */
static int
pc98_auto_scan(
	const struct partition_scheme *scheme,
	struct disk *disk,
	struct partition *entries,
	unsigned capacity)
{
	int error;
	uint8_t sector[512];

	(void)scheme;

	/* Checks the disk read result. */
	if (disk == NULL || disk->d_block_size != 512U ||
	    disk_read(disk, 0, 1, sector) != 0) {
		/* Reports operation failure. */
		return -1;
	}

	/*
	 * A PC-98 IPL may intentionally carry the PC/AT 55 aa marker as part
	 * of a dual-format disk.  The firmware's IPL1 signature is therefore
	 * authoritative when both signatures are present.
	 */
	if (sector[4] == 'I' && sector[5] == 'P' && sector[6] == 'L' &&
	    sector[7] == '1') {
		/* Computes the function result. */
		error = drv_partition_scheme_pc98.scan(
			&drv_partition_scheme_pc98, disk, entries, capacity);

		/* Failed. */
		return error;
	}

	/* Handles the sector condition. */
	if (sector[510] == 0x55U && sector[511] == 0xaaU) {
		/* Computes the function result. */
		error = drv_partition_scheme_mbr.scan(
			&drv_partition_scheme_mbr, disk, entries, capacity);

		/* Failed. */
		return error;
	}

	/* Computes the function result. */
	error = drv_partition_scheme_pc98.scan(
		&drv_partition_scheme_pc98, disk, entries, capacity);

	/* Returns the computed result. */
	return error;
}

/* Supports the pc98 scan operation. */
static int
pc98_scan(
	const struct partition_scheme *scheme,
	struct disk *dev,
	struct partition *entries,
	unsigned max_entries)
{
	char c;
	const uint8_t *p;
	struct partition *entry;
	uint64_t end_lba;
	unsigned j;
	struct disk_geometry geometry;
	uint8_t sector[512];
	unsigned count;
	unsigned i;

	(void)scheme;

	/* Checks the disk ioctl result. */
	if (dev->d_block_size != 512 ||
	    disk_ioctl(dev, DISK_IOCTL_GET_GEOMETRY, &geometry) != 0 ||
	    geometry.heads == 0 || geometry.sectors_per_track == 0) {
		/* Reports operation failure. */
		return -1;
	}

	/* Checks the disk read result. */
	if (disk_read(dev, PC98_TABLE_LBA, 1, sector) != 0)
		return -1;

	/* Checks the remaining item count. */
	count = 512U / PC98_ENTRY_SIZE;
	if (count > max_entries)
		count = max_entries;
	/* Process each remaining element. */
	for (i = 0; i < count; i++) {
		p = sector + i * PC98_ENTRY_SIZE;
		entry = &entries[i];

		/* Starts each entry out empty. */
		entry->p_parent = dev;
		entry->p_disk = NULL;
		entry->p_index = i;
		entry->p_start_block = 0;
		entry->p_data_block = 0;
		entry->p_block_count = 0;
		entry->p_flags = 0;
		entry->p_label[0] = '\0';
		entry->p_uuid[0] = '\0';

		/* Checks the current pointer. */
		if (p[0] == 0)
			continue;

		/* Checks the chs to lba result. */
		if (!chs_to_lba(&geometry, p + 4, &entry->p_start_block) ||
		    !chs_to_lba(&geometry, p + 8, &entry->p_data_block) ||
		    !chs_to_lba(&geometry, p + 12, &end_lba) ||
		    entry->p_data_block < entry->p_start_block ||
		    end_lba < entry->p_data_block ||
		    end_lba >= dev->d_block_count)
			continue;
		entry->p_block_count = end_lba - entry->p_data_block + 1U;

		/* Checks the current pointer. */
		if ((p[0] & 0x80U) && (p[1] & 0x80U))
			entry->p_flags |= PARTITION_BOOTABLE;
		/* Process each element required by the operation. */
		for (j = 0; j < 16; j++) {
			/* Classifies the current input character. */
			c = (char)p[16 + j];
			if (c == '\0' || c == ' ')
				break;
			entry->p_label[j] = c;
		}

		entry->p_label[j] = '\0';

		/* Handles the j condition. */
		if (j != 0)
			entry->p_flags |= PARTITION_HAS_LABEL;
	}

	/* Returns the computed result. */
	return (int)count;
}

/* sect, head, cyl(16bit) -> LBA in the firmware geometry. */
static int
chs_to_lba(
	const struct disk_geometry *geometry,
	const uint8_t *p,
	uint64_t *result)
{
	uint16_t cylinder = get16(p + 2);

	/* Checks the current pointer. */
	if (p[1] >= geometry->heads || p[0] >= geometry->sectors_per_track)
		return 0;
	*result = ((uint64_t)cylinder * geometry->heads + p[1]) *
			  geometry->sectors_per_track +
		  p[0];

	/* Reports operation failure. */
	return 1;
}

/* Supports the get16 operation. */
static uint16_t
get16(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
