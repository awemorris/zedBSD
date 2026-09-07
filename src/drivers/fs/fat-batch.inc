/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#define FAT_BATCH_SECTORS 8U
#define FAT_BATCH_ENTRIES 32U

struct fat_entry_change {
	uint32_t cluster;
	uint32_t value;
};

struct fat_sector_change {
	uint32_t lba;
	uint8_t old_bytes[512];
	uint8_t new_bytes[512];
};

static int fat_sector_images_commit(struct fat_mount_state *filesystem, struct fat_sector_change *slots, unsigned used, uint32_t publish_lba);
static int fat_directory_transaction(struct fat_mount_state *filesystem, const uint32_t *lbas, const uint16_t *offsets, const uint8_t entries[][32], unsigned count, int publish_last);
static int fat_batch_add_lba(uint32_t *lbas, unsigned *used, uint32_t lba);
static uint8_t *fat_batch_bytes(struct fat_sector_change *slots, unsigned used, uint32_t lba);
static int fat_table_transaction(struct fat_mount_state *filesystem, const struct fat_entry_change *changes, unsigned count, int *admitted);
static int fat_raw_set_cluster(struct fat_mount_state *filesystem, uint32_t cluster, uint32_t value);
static int fat_link_initialized_cluster(struct fat_mount_state *filesystem, uint32_t tail, uint32_t added);

/* Admit every affected physical sector before making any table changes. */
static int
fat_batch_add_lba(
	uint32_t *lbas,
	unsigned *used,
	uint32_t lba)
{
	unsigned n;

	/* Reuse one image for all changes that share a physical sector. */
	for (n = 0; n < *used; n++) {
		if (lbas[n] == lba)
			return 0;
	}

	/* Ask the caller to split before exhausting bounded transaction storage. */
	if (*used == FAT_BATCH_SECTORS)
		return E2BIG;
	lbas[(*used)++] = lba;
	return 0;
}

/* Find the private replacement image admitted during preflight. */
static uint8_t *
fat_batch_bytes(
	struct fat_sector_change *slots,
	unsigned used,
	uint32_t lba)
{
	unsigned n;

	/* Locate the exact physical identity without aliasing the mutable window. */
	for (n = 0; n < used; n++) {
		if (slots[n].lba == lba)
			return slots[n].new_bytes;
	}

	/* Report an inconsistent preflight rather than dereference a missing image. */
	return NULL;
}

/* Publish complete sector images with optional final directory-name ordering. */
static int
fat_sector_images_commit(
	struct fat_mount_state *filesystem,
	struct fat_sector_change *slots,
	unsigned used,
	uint32_t publish_lba)
{
	unsigned n;
	int error;
	int rollback;
	int restored;

	/* Make initialization or directory detachment durable before changing reachability. */
	error = disk_sync(filesystem->disk);
	if (error != 0) {
		return error;
	}
	fat_engine_invalidate(filesystem);
	if (filesystem->owner != NULL)
		io_epoch_begin(&filesystem->owner->m_write_epoch);

	/* Publish every mirror and confirm the complete table transaction. */
	for (n = 0; n < used; n++) {
		if (slots[n].lba == publish_lba)
			continue;
		error = fat_sector_write(filesystem, slots[n].lba, slots[n].new_bytes);
		if (error != 0)
			break;
	}
	/* Persist preceding LFN sectors before their final short-name publication. */
	if (error == 0 && publish_lba != UINT32_MAX) {
		if (used > 1)
			error = disk_sync(filesystem->disk);
		if (error == 0)
			error = fat_sector_write(filesystem, publish_lba, fat_batch_bytes(slots, used, publish_lba));
	}
	if (error == 0)
		error = disk_sync(filesystem->disk);

	/* Restore all captured sectors, including writes that may have completed with error. */
	rollback = 0;
	if (error != 0) {
		for (n = 0; n < used; n++) {
			restored = fat_sector_write(filesystem, slots[n].lba, slots[n].old_bytes);
			if (rollback == 0 && restored != 0)
				rollback = restored;
		}
		restored = disk_sync(filesystem->disk);
		if (rollback == 0)
			rollback = restored;
		if (rollback != 0)
			filesystem->read_only = 1;
	}
	fat_engine_invalidate(filesystem);
	if (filesystem->owner != NULL)
		io_epoch_end(&filesystem->owner->m_write_epoch);

	/* Preserve failure and freeze the mount when restoration is uncertain. */
	if (rollback != 0)
		return rollback;
	return error;
}

/* Commit bounded FAT sector images and restore every mirror on uncertain errors. */
static int
fat_table_transaction(
	struct fat_mount_state *filesystem,
	const struct fat_entry_change *changes,
	unsigned count,
	int *admitted)
{
	struct fat_sector_change *slots;
	uint32_t lbas[FAT_BATCH_SECTORS];
	uint32_t offset;
	uint32_t copy_start;
	uint32_t value;
	uint8_t *first;
	uint8_t *second;
	unsigned used;
	unsigned n;
	unsigned copy;
	unsigned entry;
	unsigned bytes;
	int error;

	/* Validate the complete bounded operation before any write or allocation. */
	*admitted = 0;
	if (filesystem->read_only)
		return EROFS;
	if (count == 0)
		return 0;
	if (count > FAT_BATCH_ENTRIES)
		return E2BIG;
	used = 0;
	bytes = filesystem->type == ZEDBSD_FAT32 ? 4U : 2U;
	for (entry = 0; entry < count; entry++) {
		if (!fat_raw_valid_cluster(filesystem, changes[entry].cluster))
			return EIO;
		offset = fat_raw_entry_offset(filesystem, changes[entry].cluster);
		for (copy = 0; copy < filesystem->number_of_fats; copy++) {
			if (copy > (UINT32_MAX - filesystem->fat_start) / filesystem->fat_sectors)
				return EIO;
			copy_start = filesystem->fat_start + copy * filesystem->fat_sectors;
			if (((uint64_t)offset + bytes - 1U) / 512U > UINT32_MAX - copy_start)
				return EIO;
			error = fat_batch_add_lba(lbas, &used, copy_start + offset / 512U);
			if (error != 0)
				return error;
			error = fat_batch_add_lba(lbas, &used, copy_start + (offset + bytes - 1U) / 512U);
			if (error != 0)
				return error;
		}
	}

	/* Decline admission without side effects when working memory is unavailable. */
	slots = kern_malloc(used * sizeof(*slots));
	if (slots == NULL)
		return ENOMEM;
	*admitted = 1;
	error = fat_engine_flush(filesystem);
	if (error != 0) {
		kern_free(slots);
		return error;
	}

	/* Preserve each mirror's actual old bytes, including unrelated packed entries. */
	for (n = 0; n < used; n++) {
		slots[n].lba = lbas[n];
		error = fat_sector_read(filesystem, lbas[n], slots[n].old_bytes);
		if (error != 0) {
			kern_free(slots);
			return error;
		}
		memcpy(slots[n].new_bytes, slots[n].old_bytes, 512);
	}

	/* Merge replacements into the private sector images without changing neighbors. */
	for (entry = 0; entry < count; entry++) {
		offset = fat_raw_entry_offset(filesystem, changes[entry].cluster);
		value = changes[entry].value;
		for (copy = 0; copy < filesystem->number_of_fats; copy++) {
			copy_start = filesystem->fat_start + copy * filesystem->fat_sectors;
			first = fat_batch_bytes(slots, used, copy_start + offset / 512U);
			second = fat_batch_bytes(slots, used, copy_start + (offset + 1U) / 512U);
			if (first == NULL || second == NULL) {
				kern_free(slots);
				return EIO;
			}
			first += offset & 511U;
			second += (offset + 1U) & 511U;
			if (filesystem->type == ZEDBSD_FAT32) {
				put32(first, (fat_engine_get32(first) & 0xf0000000U) | (value & 0x0fffffffU));
			} else if (filesystem->type == ZEDBSD_FAT16) {
				put16(first, (uint16_t)value);
			} else if (changes[entry].cluster & 1U) {
				*first = (uint8_t)((*first & 0x0fU) | ((value << 4) & 0xf0U));
				*second = (uint8_t)(value >> 4);
			} else {
				*first = (uint8_t)value;
				*second = (uint8_t)((*second & 0xf0U) | ((value >> 8) & 0x0fU));
			}
		}
	}

	/* Publish the fully staged table and release its private working images. */
	error = fat_sector_images_commit(filesystem, slots, used, UINT32_MAX);
	kern_free(slots);
	return error;
}


/* Preserve the immediate fallback with the same required durability boundaries. */
static int
fat_raw_set_cluster(
	struct fat_mount_state *filesystem,
	uint32_t cluster,
	uint32_t value)
{
	struct fat_entry_change change;
	int error;
	int admitted;

	/* Prefer one bounded transaction, including both bytes of packed FAT12 entries. */
	change.cluster = cluster;
	change.value = value;
	error = fat_table_transaction(filesystem, &change, 1, &admitted);
	if (admitted || (error != ENOMEM && error != E2BIG))
		return error;

	/* Retain all supported mirror counts and allocation-free failure recovery. */
	error = fat_raw_set_cluster_immediate(filesystem, cluster, value);

	/* Report the original update result without leaving deferred state. */
	return error;
}

/* Publish a newly initialized cluster together with a same-sector tail link. */
static int
fat_link_initialized_cluster(
	struct fat_mount_state *filesystem,
	uint32_t tail,
	uint32_t added)
{
	struct fat_entry_change changes[2];
	uint32_t first_offset;
	uint32_t second_offset;
	unsigned bytes;
	int admitted;
	int error;
	int rollback;

	/* Combine only changes that cannot expose a link to a separate pending sector. */
	first_offset = fat_raw_entry_offset(filesystem, tail);
	second_offset = fat_raw_entry_offset(filesystem, added);
	bytes = filesystem->type == ZEDBSD_FAT32 ? 4U : 2U;
	if (first_offset / 512U == second_offset / 512U &&
	    first_offset / 512U == (first_offset + bytes - 1U) / 512U &&
	    second_offset / 512U == (second_offset + bytes - 1U) / 512U) {
		changes[0].cluster = added;
		changes[0].value = fat_raw_end_of_chain(filesystem);
		changes[1].cluster = tail;
		changes[1].value = added;
		error = fat_table_transaction(filesystem, changes, 2, &admitted);
		if (admitted || (error != ENOMEM && error != E2BIG))
			return error;
	}

	/* Make a cross-sector new endpoint durable before publishing its tail link. */
	error = fat_raw_set_cluster(filesystem, added, fat_raw_end_of_chain(filesystem));
	if (error != 0)
		return error;
	error = fat_raw_set_cluster(filesystem, tail, added);
	if (error == 0)
		return 0;

	/* Recycle the new endpoint only after the failed link has been restored. */
	rollback = fat_raw_set_cluster(filesystem, added, 0);
	if (rollback != 0) {
		filesystem->read_only = 1;
		return rollback;
	}

	/* Preserve the original tail-link failure. */
	return error;
}


/* Merge a bounded directory run while preserving neighboring and hidden slots. */
static int
fat_directory_transaction(
	struct fat_mount_state *filesystem,
	const uint32_t *lbas,
	const uint16_t *offsets,
	const uint8_t entries[][32],
	unsigned count,
	int publish_last)
{
	struct fat_sector_change *slots;
	uint32_t identities[FAT_BATCH_SECTORS];
	uint8_t *bytes;
	unsigned used;
	unsigned n;
	int error;

	/* Validate and admit the complete directory run before touching its contents. */
	if (filesystem->read_only)
		return EROFS;
	if (count == 0 || count > FAT_LFN_MAX_ENTRIES + 1U)
		return EINVAL;
	used = 0;
	for (n = 0; n < count; n++) {
		if (offsets[n] > 512U - 32U || (offsets[n] & 31U) != 0)
			return EINVAL;
		error = fat_batch_add_lba(identities, &used, lbas[n]);
		if (error != 0)
			return error;
	}
	slots = kern_malloc(used * sizeof(*slots));
	if (slots == NULL)
		return ENOMEM;
	error = fat_engine_flush(filesystem);
	if (error != 0) {
		kern_free(slots);
		return error;
	}

	/* Capture full old sectors, including data behind the original end marker. */
	for (n = 0; n < used; n++) {
		slots[n].lba = identities[n];
		error = fat_sector_read(filesystem, identities[n], slots[n].old_bytes);
		if (error != 0) {
			kern_free(slots);
			return error;
		}
		memcpy(slots[n].new_bytes, slots[n].old_bytes, 512U);
	}
	for (n = 0; n < count; n++) {
		bytes = fat_batch_bytes(slots, used, lbas[n]);
		if (bytes == NULL) {
			kern_free(slots);
			return EIO;
		}
		memcpy(bytes + offsets[n], entries[n], 32U);
	}

	/* Publish the short-name sector last when creating a multi-sector LFN run. */
	error = fat_sector_images_commit(filesystem, slots, used,
	    publish_last ? lbas[count - 1U] : UINT32_MAX);
	kern_free(slots);
	return error;
}
