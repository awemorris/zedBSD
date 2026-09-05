/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/partition.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <kern/kmem.h>
#include <kern/buf.h>
#include <kern/backing-claim.h>

static const struct partition_scheme *active_scheme;
static struct partition partitions[PARTITION_POOL_MAX];
static unsigned partitions_count;
static atomic_uint_t partition_reloading;

void partition_set_scheme(const struct partition_scheme *scheme)
{
	active_scheme = scheme;
}

const struct partition_scheme *partition_get_scheme(void)
{
	return active_scheme;
}

int partition_scan(struct disk *disk, struct partition *entries,
		   unsigned capacity)
{
	if (active_scheme == NULL || active_scheme->scan == NULL ||
	    disk == NULL || entries == NULL || capacity == 0)
		return -EINVAL;
	return active_scheme->scan(active_scheme, disk, entries, capacity);
}

static int partition_name(struct partition *partition, char name[DISK_NAME_MAX])
{
	char reverse[10U];
	unsigned at = 0, count = 0, i, number;
	const char *parent = partition->p_parent->d_name;

	if (partition->p_index == UINT_MAX)
		return EOVERFLOW;
	number = partition->p_index + 1U;
	while (parent[at] != '\0' && at + 1U < DISK_NAME_MAX)
		name[at] = parent[at], at++;
	if (at == 0)
		return EINVAL;
	if (parent[at - 1U] >= '0' && parent[at - 1U] <= '9') {
		if (at + 1U >= DISK_NAME_MAX)
			return ENAMETOOLONG;
		name[at++] = 'p';
	}
	do {
		if (count == sizeof(reverse))
			return ENAMETOOLONG;
		reverse[count++] = (char)('0' + number % 10U);
		number /= 10U;
	} while (number != 0U);
	if (at + count >= DISK_NAME_MAX)
		return ENAMETOOLONG;
	for (i = 0; i < count; i++)
		name[at++] = reverse[count - i - 1U];
	name[at] = '\0';
	return 0;
}

int partition_create_disk(struct partition *source)
{
	struct partition *partition;
	struct disk *disk;
	int error;
	if (source == NULL || source->p_parent == NULL ||
	    source->p_block_count == 0 || partitions_count >= PARTITION_POOL_MAX)
		return EINVAL;
	for (unsigned slot = 0; slot < PARTITION_POOL_MAX; slot++) {
		if (partitions[slot].p_disk == NULL) {
			partition = &partitions[slot];
			goto available;
		}
	}
	return ENOSPC;
available:
	*partition = *source;
	partition->p_disk = NULL;
	disk = disk_alloc();
	if (disk == NULL)
		return ENOSPC;
	error = partition_name(partition, disk->d_name);
	if (error != 0) {
		(void)disk_destroy(disk);
		return error;
	}
	disk->d_flags = DISK_PARTITION |
		(partition->p_parent->d_flags & DISK_READ_ONLY);
	disk->d_block_size = partition->p_parent->d_block_size;
	disk->d_block_count = partition->p_block_count;
	disk->d_max_transfer_blocks = partition->p_parent->d_max_transfer_blocks;
	disk->d_parent = partition->p_parent;
	disk->d_parent_offset = partition->p_data_block;
	disk->d_data = partition;
	error = disk_create(disk);
	if (error != 0) {
		/* disk_create() acquires the parent reference only on success. */
		disk->d_parent = NULL;
		(void)disk_destroy(disk);
		return error;
	}
	partition->p_disk = disk;
	source->p_disk = disk;
	(void)atomic_raw_fetch_add_release(&partitions_count, 1);
	return 0;
}

void partition_reset(void)
{
	unsigned i;
	for (i = 0; i < PARTITION_POOL_MAX; i++) {
		partitions[i].p_disk = NULL;
		partitions[i].p_parent = NULL;
	}
	atomic_raw_store_release(&partitions_count, 0);
}

unsigned partition_count(void) { return atomic_raw_load_acquire(&partitions_count); }

const struct partition *partition_at(unsigned index)
{
	for (unsigned slot = 0; slot < PARTITION_POOL_MAX; slot++)
		if (partitions[slot].p_disk != NULL && index-- == 0)
			return &partitions[slot];
	return NULL;
}

struct reload_workspace {
	struct partition entries[PARTITION_MAX];
	struct disk *new_disks[PARTITION_MAX];
	struct partition *new_slots[PARTITION_MAX];
	uint8_t sector[4096];
};

static uint32_t reload_get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Boot's legacy MBR scanner skips bad records. Administrative reload must
 * reject the entire table instead of silently losing an invalid partition. */
static int reload_check_mbr(struct disk *disk, uint8_t *sector)
{
	unsigned i, j;
	int error = disk_read(disk, 0, 1, sector);
	if (error != 0)
		return error;
	if (sector[510] != 0x55 || sector[511] != 0xaa)
		return EINVAL;
	for (i = 0; i < 4; i++)
		if (sector[446 + 16 * i + 4] == 0xee)
			return 0; /* The strict GPT scanner owns all GPT validation. */
	if (disk->d_block_size != 512)
		return EOPNOTSUPP;
	for (i = 0; i < 4; i++) {
		const uint8_t *p = sector + 446 + i * 16;
		uint32_t start = reload_get32(p + 8), length = reload_get32(p + 12);
		if (p[4] == 0) {
			if (start || length || p[0])
				return EINVAL;
			continue;
		}
		if (p[4] == 5 || p[4] == 15 || p[4] == 0x85)
			return EOPNOTSUPP;
		if ((p[0] != 0 && p[0] != 0x80) || !start || !length ||
		    (uint64_t)start + length > disk->d_block_count)
			return EINVAL;
		for (j = 0; j < i; j++) {
			const uint8_t *q = sector + 446 + j * 16;
			uint32_t other = reload_get32(q + 8), size = reload_get32(q + 12);
			if (q[4] && start < (uint64_t)other + size &&
			    other < (uint64_t)start + length)
				return EINVAL;
		}
	}
	return 0;
}

int partition_reload(struct disk *parent)
{
	struct backing_mutation_guard guard;
	struct reload_workspace *work;
	unsigned count = 0, i, slot = 0;
	int error, scanned;
	if (parent == NULL || parent->d_parent != NULL ||
	    (parent->d_flags & DISK_PARTITION))
		return EINVAL;
	if (active_scheme == NULL ||
	    (strcmp(active_scheme->name, "pcat-auto") &&
	     strcmp(active_scheme->name, "gpt") && strcmp(active_scheme->name, "mbr")) ||
	    (parent->d_block_size != 512 && parent->d_block_size != 4096))
		return EOPNOTSUPP;
	if (!atomic_try_acquire_zero(&partition_reloading))
		return EBUSY;
	error = backing_mutation_begin_disk(parent, 0, parent->d_block_count,
	    NULL, &guard);
	if (error != 0)
		goto unlock;
	error = disk_reload_begin(parent);
	if (error != 0)
		goto release_guard;
	work = kern_calloc(1, sizeof(*work));
	if (work == NULL) {
		error = ENOMEM;
		goto end_gate;
	}
	error = buf_invalidate_disk(parent, 0);
	if (error == 0)
		error = bio_flush(parent);
	if (error == 0)
		error = reload_check_mbr(parent, work->sector);
	if (error != 0)
		goto free_work;
	scanned = partition_scan(parent, work->entries, PARTITION_MAX);
	if (scanned < 0 || scanned > (int)PARTITION_MAX) {
		error = scanned == -1 || scanned > (int)PARTITION_MAX ? EINVAL : -scanned;
		goto free_work;
	}
	for (i = 0; i < (unsigned)scanned; i++) {
		struct partition *p;
		struct disk *d;
		if (!work->entries[i].p_block_count)
			continue;
		while (slot < PARTITION_POOL_MAX && partitions[slot].p_disk != NULL)
			slot++;
		if (slot == PARTITION_POOL_MAX) {
			error = ENOSPC;
			goto free_candidates;
		}
		p = &partitions[slot++];
		*p = work->entries[i];
		p->p_disk = NULL;
		d = disk_alloc();
		if (d == NULL) {
			error = ENOSPC;
			goto free_candidates;
		}
		work->new_slots[count] = p;
		work->new_disks[count++] = d;
		error = partition_name(p, d->d_name);
		if (error != 0)
			goto free_candidates;
		d->d_flags = DISK_PARTITION | (parent->d_flags & DISK_READ_ONLY);
		d->d_block_size = parent->d_block_size;
		d->d_block_count = p->p_block_count;
		d->d_max_transfer_blocks = parent->d_max_transfer_blocks;
		d->d_parent = parent;
		d->d_parent_offset = p->p_data_block;
		d->d_data = p;
	}
	error = disk_reload_replace(parent, work->new_disks, count);
	if (error != 0)
		goto free_candidates;
	for (i = 0; i < PARTITION_POOL_MAX; i++) {
		struct partition *p = &partitions[i];
		if (p->p_disk != NULL && p->p_parent == parent) {
			/* Admission proved no external child references. */
			(void)disk_destroy(p->p_disk);
			memset(p, 0, sizeof(*p));
			(void)atomic_raw_fetch_add_release(&partitions_count, (unsigned)-1);
		}
	}
	for (i = 0; i < count; i++) {
		work->new_slots[i]->p_disk = work->new_disks[i];
		(void)atomic_raw_fetch_add_release(&partitions_count, 1);
	}
	goto free_work;
free_candidates:
	for (i = 0; i < count; i++) {
		work->new_disks[i]->d_parent = NULL; /* Not published: no parent ref. */
		(void)disk_destroy(work->new_disks[i]);
		memset(work->new_slots[i], 0, sizeof(*work->new_slots[i]));
	}
free_work:
	kern_free(work);
end_gate:
	disk_reload_end(parent);
release_guard:
	backing_mutation_end(&guard);
unlock:
	atomic_store_release(&partition_reloading, 0);
	return error;
}
