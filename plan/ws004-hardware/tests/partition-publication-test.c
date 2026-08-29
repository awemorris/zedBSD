/*
 * WS004 HW-T20 production partition publication/naming fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#include <kern/partition.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
static unsigned allocations;
static unsigned destructions;
static unsigned creates;
static int fail_alloc;
static int create_error;
static int destroy_saw_parent;
static struct disk *published;

#define CHECK(expression) do { \
	if (!(expression)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

struct disk *
disk_alloc(void)
{
	struct disk *result;

	if (fail_alloc)
		return NULL;
	result = calloc(1U, sizeof(*result));
	if (result != NULL)
		allocations++;
	return result;
}

int
disk_create(struct disk *disk)
{
	creates++;
	if (create_error != 0)
		return create_error;
	published = disk;
	return 0;
}

int
disk_destroy(struct disk *disk)
{
	if (disk == NULL)
		return EINVAL;
	if (disk->d_parent != NULL)
		destroy_saw_parent = 1;
	if (published == disk)
		published = NULL;
	destructions++;
	free(disk);
	return 0;
}

static void
reset_state(void)
{
	partition_reset();
	allocations = 0U;
	destructions = 0U;
	creates = 0U;
	fail_alloc = 0;
	create_error = 0;
	destroy_saw_parent = 0;
	published = NULL;
}

static struct partition
make_partition(struct disk *parent, unsigned index)
{
	struct partition result;

	memset(&result, 0, sizeof(result));
	result.p_parent = parent;
	result.p_index = index;
	result.p_start_block = 100U;
	result.p_data_block = 101U;
	result.p_block_count = 25U;
	return result;
}

static void
release_published(void)
{
	struct disk *disk = published;

	partition_reset();
	if (disk != NULL)
		(void)disk_destroy(disk);
}

static void
test_nvme_decimal_partition_name(void)
{
	struct disk parent;
	struct partition source;

	reset_state();
	memset(&parent, 0, sizeof(parent));
	strcpy(parent.d_name, "nvme0n1");
	parent.d_block_size = 4096U;
	parent.d_max_transfer_blocks = 32U;
	source = make_partition(&parent, 99U);
	CHECK(partition_create_disk(&source) == 0);
	CHECK(published != NULL);
	CHECK(strcmp(published->d_name, "nvme0n1p100") == 0);
	CHECK(published->d_parent == &parent);
	CHECK(published->d_parent_offset == 101U);
	CHECK(published->d_block_count == 25U);
	CHECK(published->d_block_size == 4096U);
	CHECK(source.p_disk == published);
	CHECK(partition_count() == 1U);
	CHECK(allocations == 1U && creates == 1U && destructions == 0U);
	release_published();
	CHECK(destructions == 1U);
}

static void
test_non_numeric_parent_name(void)
{
	struct disk parent;
	struct partition source;

	reset_state();
	memset(&parent, 0, sizeof(parent));
	strcpy(parent.d_name, "sda");
	parent.d_block_size = 512U;
	source = make_partition(&parent, 99U);
	CHECK(partition_create_disk(&source) == 0);
	CHECK(published != NULL);
	CHECK(strcmp(published->d_name, "sda100") == 0);
	release_published();
}

static void
test_name_failure_releases_allocation(void)
{
	struct disk parent;
	struct partition source;

	reset_state();
	memset(&parent, 0, sizeof(parent));
	memset(parent.d_name, '9', sizeof(parent.d_name) - 1U);
	parent.d_name[sizeof(parent.d_name) - 1U] = '\0';
	source = make_partition(&parent, 0U);
	CHECK(partition_create_disk(&source) == ENAMETOOLONG);
	CHECK(allocations == 1U && destructions == 1U && creates == 0U);
	CHECK(partition_count() == 0U);
	CHECK(source.p_disk == NULL);

	reset_state();
	memset(&parent, 0, sizeof(parent));
	strcpy(parent.d_name, "nvme0n1");
	source = make_partition(&parent, UINT_MAX);
	CHECK(partition_create_disk(&source) == EOVERFLOW);
	CHECK(allocations == 1U && destructions == 1U && creates == 0U);
	CHECK(partition_count() == 0U);
}

static void
test_create_failure_releases_without_parent_ref(void)
{
	struct disk parent;
	struct partition source;

	reset_state();
	memset(&parent, 0, sizeof(parent));
	strcpy(parent.d_name, "nvme0n1");
	source = make_partition(&parent, 3U);
	create_error = EIO;
	CHECK(partition_create_disk(&source) == EIO);
	CHECK(allocations == 1U && destructions == 1U && creates == 1U);
	CHECK(destroy_saw_parent == 0);
	CHECK(partition_count() == 0U);
	CHECK(source.p_disk == NULL);
}

static void
test_allocation_failure_is_transactional(void)
{
	struct disk parent;
	struct partition source;

	reset_state();
	memset(&parent, 0, sizeof(parent));
	strcpy(parent.d_name, "nvme0n1");
	source = make_partition(&parent, 0U);
	fail_alloc = 1;
	CHECK(partition_create_disk(&source) == ENOSPC);
	CHECK(allocations == 0U && destructions == 0U && creates == 0U);
	CHECK(partition_count() == 0U);
	CHECK(source.p_disk == NULL);
}

int
main(void)
{
	test_nvme_decimal_partition_name();
	test_non_numeric_parent_name();
	test_name_failure_releases_allocation();
	test_create_failure_releases_without_parent_ref();
	test_allocation_failure_is_transactional();
	partition_reset();
	if (failures != 0U) {
		printf("HW-T20 partition publication: %u failure(s)\n", failures);
		return 1;
	}
	puts("HW-T20 partition publication: PASS (pN naming and rollback)");
	return 0;
}
