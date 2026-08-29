/* PC/AT per-disk GPT versus legacy MBR selection.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include <drivers/disklabel.h>

#include <errno.h>
#include <kern/kmem.h>
#include <string.h>

#define MBR_TABLE 0x1beU
#define MBR_ENTRY_SIZE 16U

static int
pcat_auto_scan(const struct partition_scheme *scheme, struct disk *disk,
	struct partition *entries, unsigned capacity)
{
	uint8_t *block;
	unsigned slot;
	int has_protective = 0;
	int has_gpt_signature = 0;
	int error;

	(void)scheme;
	if (disk == NULL || entries == NULL || capacity == 0U ||
	    (disk->d_block_size != 512U && disk->d_block_size != 4096U))
		return -EINVAL;
	block = kern_malloc(disk->d_block_size);
	if (block == NULL)
		return -ENOMEM;
	if (disk_read(disk, 0U, 1U, block) != 0) {
		error = -EIO;
		goto out;
	}
	if (block[510U] != 0x55U || block[511U] != 0xaaU) {
		error = -EINVAL;
		goto out;
	}
	for (slot = 0U; slot < 4U; slot++)
		if (block[MBR_TABLE + slot * MBR_ENTRY_SIZE + 4U] == 0xeeU) {
			has_protective = 1;
			break;
		}
	/* The protective entry is already sufficient GPT evidence.  Enter the
	 * strict parser immediately so that it can recover from an unreadable
	 * primary header by validating the backup copy. */
	if (has_protective) {
		kern_free(block);
		return partition_scheme_gpt.scan(&partition_scheme_gpt, disk,
		    entries, capacity);
	}
	if (disk->d_block_count <= 1U || disk_read(disk, 1U, 1U, block) != 0) {
		error = -EIO;
		goto out;
	}
	if (memcmp(block, "EFI PART", 8U) == 0)
		has_gpt_signature = 1;
	if (!has_gpt_signature) {
		if (disk_read(disk, disk->d_block_count - 1U, 1U, block) != 0) {
			error = -EIO;
			goto out;
		}
		if (memcmp(block, "EFI PART", 8U) == 0)
			has_gpt_signature = 1;
	}
	kern_free(block);
	/* Any EE entry or GPT header signature is GPT evidence.  Once selected,
	 * strict GPT rejection is final and must never fall back to legacy MBR. */
	if (has_gpt_signature)
		return partition_scheme_gpt.scan(&partition_scheme_gpt, disk,
		    entries, capacity);
	return partition_scheme_mbr.scan(&partition_scheme_mbr, disk, entries,
	    capacity);
out:
	kern_free(block);
	return error;
}

const struct partition_scheme partition_scheme_pcat_auto = {
	.name = "pcat-auto",
	.scan = pcat_auto_scan,
};
