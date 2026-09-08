/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Big-endian Sun disklabel partition scanner.
 */

#include <drivers/disklabel.h>

static int scan(const struct partition_scheme *s, struct disk *d, struct partition *e, unsigned capacity);
static uint16_t be16(const uint8_t *p);
static uint32_t be32(const uint8_t *p);

const struct partition_scheme drv_partition_scheme_sun = {
	.name = "sun",
	.scan = scan
};

/* Supports the scan operation. */
static int
scan(
	const struct partition_scheme *s,
	struct disk *d,
	struct partition *e,
	unsigned capacity)
{
	uint64_t start;
	uint64_t blocks;
	uint8_t sector[512];
	uint16_t sum = 0, heads, sectors;
	unsigned count = capacity < 8U ? capacity : 8U, i;

	(void)s;

	/* Checks the disk read result. */
	if (d->d_block_size != 512U || disk_read(d, 0, 1, sector) != 0 ||
	    be16(sector + 508) != 0xdabeU) {
		/* Reports operation failure. */
		return -1;
	}
	/* Process each element required by the operation. */
	for (i = 0; i < 256U; i++)
		sum ^= be16(sector + i * 2U);

	/* Handles the sum condition. */
	if (sum != 0)
		return -1;
	heads = be16(sector + 436);

	/* Handles the heads condition. */
	sectors = be16(sector + 438);
	if (!heads || !sectors)
		return -1;
	/* Process each remaining element. */
	for (i = 0; i < count; i++) {
		start = (uint64_t)be32(sector + 444U + i * 8U) * heads *
			sectors;
		blocks = be32(sector + 448U + i * 8U);
		e[i].p_parent = d;
		e[i].p_disk = NULL;
		e[i].p_index = i;
		e[i].p_start_block = start;
		e[i].p_data_block = start;
		e[i].p_block_count = 0;
		e[i].p_flags = 0;
		e[i].p_uuid[0] = '\0';
		e[i].p_label[0] = 's';
		e[i].p_label[1] = 'l';
		e[i].p_label[2] = 'i';
		e[i].p_label[3] = 'c';
		e[i].p_label[4] = 'e';
		e[i].p_label[5] = (char)('a' + i);
		e[i].p_label[6] = '\0';

		/* Handles the blocks condition. */
		if (blocks && start < d->d_block_count &&
		    blocks <= d->d_block_count - start)
			e[i].p_block_count = blocks;

		/* Checks the current index. */
		if (i == 1U)
			e[i].p_flags |= PARTITION_BOOTABLE;
	}

	/* Returns the computed result. */
	return (int)count;
}

/* Supports the be16 operation. */
static uint16_t
be16(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* Supports the be32 operation. */
static uint32_t
be32(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}
