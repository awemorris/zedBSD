/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * PC/AT MBR primary partition scheme.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <drivers/disklabel.h>

#define MBR_TABLE 0x1beU
#define MBR_ENTRY_SIZE 16U

static uint32_t get32(const uint8_t *p);
static char hex(unsigned value);
static void mbr_partuuid(char output[PARTITION_UUID_MAX], uint32_t signature, unsigned index);
static int mbr_scan(const struct partition_scheme *scheme, struct disk *disk, struct partition *entries, unsigned capacity);

const struct partition_scheme drv_partition_scheme_mbr = {
	.name = "mbr",
	.scan = mbr_scan,
};






/* Supports the get32 operation. */
static uint32_t
get32(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* Supports the hex operation. */
static char
hex(
	unsigned value)
{
	/* Returns the computed result. */
	return (char)(value < 10U ? '0' + value : 'a' + value - 10U);
}

/* Supports the mbr partuuid operation. */
static void
mbr_partuuid(
	char output[PARTITION_UUID_MAX],
	uint32_t signature,
	unsigned index)
{
	unsigned at = 0, shift;

	/* Handles the signature condition. */
	if (signature == 0) {
		output[0] = '\0';

		/* Returns the computed result. */
		return;
	}
	/* Process each element required by the operation. */
	for (shift = 32; shift != 0; shift -= 4)
		output[at++] = hex((signature >> (shift - 4)) & 15U);
	output[at++] = '-';
	output[at++] = hex(((index + 1U) >> 4) & 15U);
	output[at++] = hex((index + 1U) & 15U);
	output[at] = '\0';
}

/* Supports the mbr scan operation. */
static int
mbr_scan(
	const struct partition_scheme *scheme,
	struct disk *disk,
	struct partition *entries,
	unsigned capacity)
{
	const uint8_t *raw;
	struct partition *entry;
	uint32_t start, blocks;
	uint8_t type;
	unsigned index_for;
	uint8_t sector[512];
	uint32_t signature;
	unsigned count = capacity < 4U ? capacity : 4U;

	(void)scheme;

	/* Checks the disk read result. */
	if (disk->d_block_size != 512U || disk_read(disk, 0, 1, sector) != 0)
		return -1;

	/* Handles the sector condition. */
	if (sector[510] != 0x55U || sector[511] != 0xaaU)
		return -1;
	signature = get32(sector + 0x1b8U);
	/* Process each remaining element. */
	for (index_for = 0; index_for < count; index_for++) {
		raw = sector + MBR_TABLE + index_for * MBR_ENTRY_SIZE;
		entry = &entries[index_for];
		start = get32(raw + 8);
		blocks = get32(raw + 12);
		type = raw[4];
		entry->p_parent = disk;
		entry->p_disk = 0;
		entry->p_index = index_for;
		entry->p_start_block = start;
		entry->p_data_block = start;
		entry->p_block_count = 0;
		entry->p_flags = 0;
		entry->p_label[0] = 'm';
		entry->p_label[1] = 'b';
		entry->p_label[2] = 'r';
		entry->p_label[3] = (char)('1' + index_for);
		entry->p_label[4] = '\0';
		mbr_partuuid(entry->p_uuid, signature, index_for);

		/* Handles the entry condition. */
		if (entry->p_uuid[0] != '\0')
			entry->p_flags |= PARTITION_HAS_UUID;

		/* Handles the raw condition. */
		if (raw[0] != 0 && raw[0] != 0x80U)
			continue;

		/* Handles the type condition. */
		if (type == 0 || blocks == 0 || type == 0x05U ||
		    type == 0x0fU || type == 0x85U || type == 0xeeU)
			continue;

		/* Handles the uint64 t condition. */
		if ((uint64_t)start + blocks > disk->d_block_count)
			continue;
		entry->p_block_count = blocks;

		/* Handles the raw condition. */
		if (raw[0] == 0x80U)
			entry->p_flags |= PARTITION_BOOTABLE;
	}

	/* Returns the computed result. */
	return (int)count;
}
