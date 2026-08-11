/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/partition.h"

#include <errno.h>

static const struct partition_scheme *active_scheme;
static struct partition partitions[PARTITION_POOL_MAX];
static unsigned partitions_count;

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
	unsigned at = 0, number = partition->p_index + 1U;
	const char *parent = partition->p_parent->d_name;
	while (parent[at] != '\0' && at + 1U < DISK_NAME_MAX)
		name[at] = parent[at], at++;
	if (at + 2U >= DISK_NAME_MAX)
		return ENAMETOOLONG;
	name[at++] = 'p';
	if (number >= 10U) {
		if (at + 2U >= DISK_NAME_MAX)
			return ENAMETOOLONG;
		name[at++] = (char)('0' + number / 10U);
	}
	name[at++] = (char)('0' + number % 10U);
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
	partition = &partitions[partitions_count];
	*partition = *source;
	partition->p_disk = NULL;
	disk = disk_alloc();
	if (disk == NULL)
		return ENOSPC;
	error = partition_name(partition, disk->d_name);
	if (error != 0)
		return error;
	disk->d_flags = DISK_PARTITION |
		(partition->p_parent->d_flags & DISK_READ_ONLY);
	disk->d_block_size = partition->p_parent->d_block_size;
	disk->d_block_count = partition->p_block_count;
	disk->d_max_transfer_blocks = partition->p_parent->d_max_transfer_blocks;
	disk->d_parent = partition->p_parent;
	disk->d_parent_offset = partition->p_data_block;
	disk->d_data = partition;
	error = disk_create(disk);
	if (error != 0)
		return error;
	partition->p_disk = disk;
	source->p_disk = disk;
	partitions_count++;
	return 0;
}

void partition_reset(void)
{
	unsigned i;
	for (i = 0; i < partitions_count; i++) {
		partitions[i].p_disk = NULL;
		partitions[i].p_parent = NULL;
	}
	partitions_count = 0;
}

unsigned partition_count(void) { return partitions_count; }

const struct partition *partition_at(unsigned index)
{
	return index < partitions_count ? &partitions[index] : NULL;
}
