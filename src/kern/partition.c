/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Partition scanning and partition disks.
 *
 * The active partition scheme scans a whole disk into partition entries;
 * each entry can then be published as a child disk that maps a block
 * range of its parent.
 */

#include "kern/partition.h"
#include "kern/kmem.h"
#include "kern/buf.h"
#include "kern/backing-claim.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

struct reload_workspace {
	struct partition entries[PARTITION_MAX];
	struct disk *new_disks[PARTITION_MAX];
	struct partition *new_slots[PARTITION_MAX];
	uint8_t sector[4096];
};

static const struct partition_scheme *active_scheme;
static struct partition partitions[PARTITION_POOL_MAX];
static unsigned partitions_count;
static atomic_uint_t partition_reloading;

static int partition_name(struct partition *partition, char name[DISK_NAME_MAX]);
static uint32_t reload_get32(const uint8_t *bytes);
static int reload_check_mbr(struct disk *disk, uint8_t *sector);
static int reload_prepare_candidates(struct disk *parent, struct reload_workspace *work, unsigned scanned, unsigned *count);
static void reload_release_candidates(struct reload_workspace *work, unsigned count);
static int partition_reload_owned(struct disk *parent, struct reload_workspace *work);
static int partition_create_owned(struct partition *source);

/*
 * Selects the partition scheme used by later scans.
 */
void
partition_set_scheme(
	const struct partition_scheme *scheme)
{
	active_scheme = scheme;
}

/*
 * Reports the active partition scheme.
 */
const struct partition_scheme *
partition_get_scheme(
	void)
{
	/* Reports the selected scheme. */
	return active_scheme;
}

/*
 * Scans a disk into partition entries with the active scheme.
 *
 * Reports the entry count, or a negated errno like the scheme callbacks.
 */
int
partition_scan(
	struct disk *disk,
	struct partition *entries,
	unsigned capacity)
{
	int count;

	/* Rejects a missing scheme, disk, or entry table. */
	if (active_scheme == NULL ||
	    active_scheme->scan == NULL ||
	    disk == NULL ||
	    entries == NULL ||
	    capacity == 0)
		return -EINVAL;

	/* Delegates to the scheme. */
	count = active_scheme->scan(active_scheme, disk, entries, capacity);

	/* Reports the scan result. */
	return count;
}

/*
 * Publishes a scanned partition as a child disk of its parent.
 *
 * The entry is copied into the partition pool and the new disk points at
 * that copy; the caller's entry also learns the disk.
 */
int
partition_create_disk(
	struct partition *source)
{
	int error;

	/* Excludes concurrent pool replacement and media retirement. */
	if (!atomic_try_acquire_zero(&partition_reloading))
		return EBUSY;
	error = partition_create_owned(source);
	atomic_store_release(&partition_reloading, 0);

	/* Reports publication without retaining the pool reservation. */
	return error;
}

/*
 * Retires an idle old medium and its pooled partition records together.
 */
int
partition_retire_media(
	struct disk *parent)
{
	unsigned i;
	int error;

	/* Keeps old partition slots reserved until registry retirement finishes. */
	if (!atomic_try_acquire_zero(&partition_reloading))
		return EBUSY;
	error = disk_media_retire(parent);
	if (error == 0) {
		/* Clears records only after every old child has been withdrawn. */
		for (i = 0; i < PARTITION_POOL_MAX; i++) {
			if (partitions[i].p_disk != NULL &&
			    partitions[i].p_parent == parent) {
				memset(&partitions[i], 0, sizeof(partitions[i]));
				(void)atomic_raw_fetch_add_release(&partitions_count, (unsigned)-1);
			}
		}
	}
	atomic_store_release(&partition_reloading, 0);

	/* Preserves all pool records when retirement was refused. */
	return error;
}

/* Publishes one partition while the caller excludes pool replacement. */
static int
partition_create_owned(
	struct partition *source)
{
	struct partition *partition;
	struct disk *disk;
	unsigned slot;
	int error;

	/* Rejects an empty entry, a parentless one, or a full pool. */
	if (source == NULL ||
	    source->p_parent == NULL ||
	    source->p_block_count == 0 ||
	    partitions_count >= PARTITION_POOL_MAX)
		return EINVAL;

	/* Finds a free slot even when a prior reload left holes in the pool. */
	partition = NULL;
	for (slot = 0; slot < PARTITION_POOL_MAX; slot++) {
		if (partitions[slot].p_disk == NULL) {
			partition = &partitions[slot];
			break;
		}
	}
	if (partition == NULL)
		return ENOSPC;

	/* Copies the entry into the reserved pool slot. */
	*partition = *source;
	partition->p_disk = NULL;

	/* Allocates the child disk. */
	disk = disk_alloc();
	if (disk == NULL)
		return ENOSPC;

	/* Names the child after its parent and partition number. */
	error = partition_name(partition, disk->d_name);
	if (error != 0) {
		(void)disk_destroy(disk);
		return error;
	}

	/* Maps the child onto the parent's block range. */
	disk->d_flags = DISK_PARTITION |
		(partition->p_parent->d_flags & DISK_READ_ONLY);
	disk->d_block_size = partition->p_parent->d_block_size;
	disk->d_block_count = partition->p_block_count;
	disk->d_max_transfer_blocks = partition->p_parent->d_max_transfer_blocks;
	disk->d_parent = partition->p_parent;
	disk->d_parent_offset = partition->p_data_block;
	disk->d_data = partition;

	/* Registers the child disk. */
	error = disk_create(disk);
	if (error != 0) {
		/* disk_create() acquires the parent reference only on success. */
		disk->d_parent = NULL;
		(void)disk_destroy(disk);
		return error;
	}

	/* Publishes the disk through both copies of the entry. */
	partition->p_disk = disk;
	source->p_disk = disk;
	(void)atomic_raw_fetch_add_release(&partitions_count, 1);

	/* Reports the published partition. */
	return 0;
}

/*
 * Forgets every pooled partition.
 */
void
partition_reset(
	void)
{
	unsigned i;

	/* Detaches every pooled entry from its disks. */
	for (i = 0; i < PARTITION_POOL_MAX; i++) {
		partitions[i].p_disk = NULL;
		partitions[i].p_parent = NULL;
	}
	atomic_raw_store_release(&partitions_count, 0);
}

/*
 * Reports the number of pooled partitions.
 */
unsigned
partition_count(
	void)
{
	/* Reports the pool fill. */
	return atomic_raw_load_acquire(&partitions_count);
}

/*
 * Reports one pooled partition by index.
 */
const struct partition *
partition_at(
	unsigned index)
{
	unsigned slot;

	/* Enumerates only published records, skipping holes left by replacement. */
	for (slot = 0; slot < PARTITION_POOL_MAX; slot++) {
		if (partitions[slot].p_disk != NULL && index-- == 0)
			return &partitions[slot];
	}

	/* Reports an index beyond the published records. */
	return NULL;
}

/*
 * Reloads an idle physical disk's validated partition table atomically.
 */
int
partition_reload(
	struct disk *parent)
{
	struct backing_mutation_guard guard;
	struct reload_workspace *work;
	int error;

	/* Restricts administration to supported physical disks and table schemes. */
	if (parent == NULL || parent->d_parent != NULL ||
	    (parent->d_flags & DISK_PARTITION) != 0)
		return EINVAL;
	if (active_scheme == NULL ||
	    (strcmp(active_scheme->name, "pcat-auto") != 0 &&
	     strcmp(active_scheme->name, "gpt") != 0 &&
	     strcmp(active_scheme->name, "mbr") != 0) ||
	    (parent->d_block_size != 512 && parent->d_block_size != 4096))
		return EOPNOTSUPP;

	/* Serializes pool replacement before taking backing and disk reservations. */
	if (!atomic_try_acquire_zero(&partition_reloading))
		return EBUSY;
	work = NULL;
	error = backing_mutation_begin_disk(
		parent, 0, parent->d_block_count, NULL, &guard);
	if (error != 0) {
		atomic_store_release(&partition_reloading, 0);
		return error;
	}
	error = disk_reload_begin(parent);
	if (error != 0) {
		backing_mutation_end(&guard);
		atomic_store_release(&partition_reloading, 0);
		return error;
	}

	/* Allocates one bounded workspace while the disk's admission is closed. */
	work = kern_calloc(1, sizeof(*work));
	if (work == NULL) {
		disk_reload_end(parent);
		backing_mutation_end(&guard);
		atomic_store_release(&partition_reloading, 0);
		return ENOMEM;
	}
	error = partition_reload_owned(parent, work);

	/* Releases every reservation after either rollback or complete publication. */
	kern_free(work);
	disk_reload_end(parent);
	backing_mutation_end(&guard);
	atomic_store_release(&partition_reloading, 0);

	/* Preserves the validation, allocation or commit result. */
	return error;
}

/* Reads a little-endian MBR word without alignment assumptions. */
static uint32_t
reload_get32(
	const uint8_t *bytes)
{
	/* Composes the on-disk word from its four bytes. */
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	    (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* Rejects the entire administrative table when any legacy record is invalid. */
static int
reload_check_mbr(
	struct disk *disk,
	uint8_t *sector)
{
	const uint8_t *entry;
	const uint8_t *earlier;
	uint32_t start;
	uint32_t length;
	uint32_t other;
	uint32_t size;
	unsigned i;
	unsigned j;
	int error;

	/* Reads and validates the protective or legacy MBR signature. */
	error = disk_read(disk, 0, 1, sector);
	if (error != 0)
		return error;
	if (sector[510] != 0x55 || sector[511] != 0xaa)
		return EINVAL;

	/* Leaves protective-table validation to the strict GPT scanner. */
	for (i = 0; i < 4; i++) {
		if (sector[446 + 16 * i + 4] == 0xee)
			return 0;
	}
	if (disk->d_block_size != 512)
		return EOPNOTSUPP;

	/* Checks every present entry and every earlier extent for overlap. */
	for (i = 0; i < 4; i++) {
		entry = sector + 446 + i * 16;
		start = reload_get32(entry + 8);
		length = reload_get32(entry + 12);
		if (entry[4] == 0) {
			if (start != 0 || length != 0 || entry[0] != 0)
				return EINVAL;
			continue;
		}
		if (entry[4] == 5 || entry[4] == 15 || entry[4] == 0x85)
			return EOPNOTSUPP;
		if ((entry[0] != 0 && entry[0] != 0x80) ||
		    start == 0 || length == 0 ||
		    (uint64_t)start + length > disk->d_block_count)
			return EINVAL;

		/* Compares this valid extent with all previous nonempty entries. */
		for (j = 0; j < i; j++) {
			earlier = sector + 446 + j * 16;
			other = reload_get32(earlier + 8);
			size = reload_get32(earlier + 12);
			if (earlier[4] != 0 &&
			    start < (uint64_t)other + size &&
			    other < (uint64_t)start + length)
				return EINVAL;
		}
	}

	/* Reports a completely valid legacy table. */
	return 0;
}

/* Builds unpublished child disks while retaining the original namespace. */
static int
reload_prepare_candidates(
	struct disk *parent,
	struct reload_workspace *work,
	unsigned scanned,
	unsigned *count)
{
	struct partition *partition;
	struct disk *disk;
	unsigned slot;
	unsigned i;
	int error;

	/* Reserves distinct free pool slots for every nonempty scanned extent. */
	slot = 0;
	for (i = 0; i < scanned; i++) {
		if (work->entries[i].p_block_count == 0)
			continue;
		while (slot < PARTITION_POOL_MAX && partitions[slot].p_disk != NULL)
			slot++;
		if (slot == PARTITION_POOL_MAX)
			return ENOSPC;
		partition = &partitions[slot++];
		*partition = work->entries[i];
		partition->p_disk = NULL;

		/* Registers ownership before any later candidate preparation can fail. */
		disk = disk_alloc();
		if (disk == NULL)
			return ENOSPC;
		work->new_slots[*count] = partition;
		work->new_disks[(*count)++] = disk;
		error = partition_name(partition, disk->d_name);
		if (error != 0)
			return error;

		/* Maps the unpublished disk with the parent's existing sector geometry. */
		disk->d_flags = DISK_PARTITION | (parent->d_flags & DISK_READ_ONLY);
		disk->d_block_size = parent->d_block_size;
		disk->d_block_count = partition->p_block_count;
		disk->d_max_transfer_blocks = parent->d_max_transfer_blocks;
		disk->d_parent = parent;
		disk->d_parent_offset = partition->p_data_block;
		disk->d_data = partition;
	}

	/* Reports that every candidate is ready for one namespace commit. */
	return 0;
}

/* Releases unpublished candidates without dropping unowned parent references. */
static void
reload_release_candidates(
	struct reload_workspace *work,
	unsigned count)
{
	unsigned i;

	/* Publication has not acquired parent references on any candidate. */
	for (i = 0; i < count; i++) {
		work->new_disks[i]->d_parent = NULL;
		(void)disk_destroy(work->new_disks[i]);
		memset(work->new_slots[i], 0, sizeof(*work->new_slots[i]));
	}
}

/* Performs reload while backing exclusion and the disk reservation are owned. */
static int
partition_reload_owned(
	struct disk *parent,
	struct reload_workspace *work)
{
	struct partition *partition;
	unsigned count;
	unsigned i;
	int scanned;
	int error;

	/* Invalidates stale sectors and validates the current on-disk table. */
	error = buf_invalidate_disk(parent, 0);
	if (error != 0)
		return error;
	error = bio_flush(parent);
	if (error != 0)
		return error;
	error = reload_check_mbr(parent, work->sector);
	if (error != 0)
		return error;
	scanned = partition_scan(parent, work->entries, PARTITION_MAX);
	if (scanned < 0 || scanned > (int)PARTITION_MAX) {
		if (scanned == -1 || scanned > (int)PARTITION_MAX)
			return EINVAL;
		return -scanned;
	}

	/* Prepares candidates and commits only after every fallible check succeeds. */
	count = 0;
	error = reload_prepare_candidates(parent, work, (unsigned)scanned, &count);
	if (error == 0)
		error = disk_reload_replace(parent, work->new_disks, count);
	if (error != 0) {
		reload_release_candidates(work, count);
		return error;
	}

	/* The admission check proved that old children have no external owners. */
	for (i = 0; i < PARTITION_POOL_MAX; i++) {
		partition = &partitions[i];
		if (partition->p_disk != NULL && partition->p_parent == parent) {
			(void)disk_destroy(partition->p_disk);
			memset(partition, 0, sizeof(*partition));
			(void)atomic_raw_fetch_add_release(&partitions_count, (unsigned)-1);
		}
	}

	/* Publishes the new immutable records after retiring the old generations. */
	for (i = 0; i < count; i++) {
		work->new_slots[i]->p_disk = work->new_disks[i];
		(void)atomic_raw_fetch_add_release(&partitions_count, 1);
	}

	/* Reports the complete replacement. */
	return 0;
}

/* Builds the child disk name from the parent name and partition number. */
static int
partition_name(
	struct partition *partition,
	char name[DISK_NAME_MAX])
{
	char reverse[10U];
	const char *parent;
	unsigned at;
	unsigned count;
	unsigned i;
	unsigned number;

	parent = partition->p_parent->d_name;
	at = 0;
	count = 0;

	/* Rejects a partition index that cannot be numbered. */
	if (partition->p_index == UINT_MAX)
		return EOVERFLOW;
	number = partition->p_index + 1U;

	/* Copies the parent name. */
	while (parent[at] != '\0' && at + 1U < DISK_NAME_MAX) {
		name[at] = parent[at];
		at++;
	}
	if (at == 0)
		return EINVAL;

	/* Separates a parent name ending in a digit with a 'p'. */
	if (parent[at - 1U] >= '0' && parent[at - 1U] <= '9') {
		if (at + 1U >= DISK_NAME_MAX)
			return ENAMETOOLONG;
		name[at++] = 'p';
	}

	/* Renders the partition number in reverse. */
	do {
		if (count == sizeof(reverse))
			return ENAMETOOLONG;
		reverse[count++] = (char)('0' + number % 10U);
		number /= 10U;
	} while (number != 0U);

	/* Appends the number in display order. */
	if (at + count >= DISK_NAME_MAX)
		return ENAMETOOLONG;
	for (i = 0; i < count; i++)
		name[at++] = reverse[count - i - 1U];
	name[at] = '\0';

	/* Reports the completed name. */
	return 0;
}
