/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * PC/AT per-disk GPT versus legacy MBR selection.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include <drivers/disklabel.h>

#include <errno.h>
#include <kern/kmem.h>
#include <string.h>

#define MBR_TABLE 0x1beU
#define MBR_ENTRY_SIZE 16U

static int pcat_auto_scan(const struct partition_scheme *scheme, struct disk *disk, struct partition *entries, unsigned capacity);

const struct partition_scheme drv_partition_scheme_pcat_auto = {
	.name = "pcat-auto",
	.scan = pcat_auto_scan,
};

/* Supports the pcat auto scan operation. */
static int
pcat_auto_scan(
	const struct partition_scheme *scheme,
	struct disk *disk,
	struct partition *entries,
	unsigned capacity)
{
	int function_result;
	uint8_t *block;
	unsigned slot;
	int has_protective = 0;
	int has_gpt_signature = 0;
	int error;

	(void)scheme;

	/* Handles the disk availability. */
	if (disk == NULL || entries == NULL || capacity == 0U ||
	    (disk->d_block_size != 512U && disk->d_block_size != 4096U)) {
		/* Failed. */
		return -EINVAL;
	}

	/* Handles the block availability. */
	block = kern_malloc(disk->d_block_size);
	if (block == NULL)
		return -ENOMEM;

	/* Checks the disk read result. */
	if (disk_read(disk, 0U, 1U, block) != 0) {
		error = -EIO;
		goto out;
	}

	/* Handles the block condition. */
	if (block[510U] != 0x55U || block[511U] != 0xaaU) {
		error = -EINVAL;
		goto out;
	}

	/* Process each element required by the operation. */
	for (slot = 0U; slot < 4U; slot++) {
		/* Handles the block condition. */
		if (block[MBR_TABLE + slot * MBR_ENTRY_SIZE + 4U] == 0xeeU) {
			has_protective = 1;
			break;
		}
	}

	/*
	 * The protective entry is already sufficient GPT evidence.  Enter the
	 * strict parser immediately so that it can recover from an unreadable
	 * primary header by validating the backup copy.
	 */
	if (has_protective) {
		kern_free(block);

		/* Computes the function result. */
		function_result = drv_partition_scheme_gpt.scan(
			&drv_partition_scheme_gpt, disk, entries, capacity);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the disk read result. */
	if (disk->d_block_count <= 1U || disk_read(disk, 1U, 1U, block) != 0) {
		error = -EIO;
		goto out;
	}

	/* Handles the memcmp condition. */
	if (memcmp(block, "EFI PART", 8U) == 0)
		has_gpt_signature = 1;

	/* Handles the gpt signature condition. */
	if (!has_gpt_signature) {
		/* Checks the disk read result. */
		if (disk_read(disk, disk->d_block_count - 1U, 1U, block) != 0) {
			error = -EIO;
			goto out;
		}

		/* Handles the memcmp condition. */
		if (memcmp(block, "EFI PART", 8U) == 0)
			has_gpt_signature = 1;
	}

	kern_free(block);

	/*
	 * Any EE entry or GPT header signature is GPT evidence.  Once selected,
	 * strict GPT rejection is final and must never fall back to legacy MBR.
	 */
	if (has_gpt_signature) {
		/* Computes the function result. */
		function_result = drv_partition_scheme_gpt.scan(
			&drv_partition_scheme_gpt, disk, entries, capacity);

		/* Returns the computed result. */
		return function_result;
	}

	/* Computes the function result. */
	function_result = drv_partition_scheme_mbr.scan(
		&drv_partition_scheme_mbr, disk, entries, capacity);

	/* Returns the computed result. */
	return function_result;
out:
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
