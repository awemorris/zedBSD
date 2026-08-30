/*
 * KA-T030/KA-T031: filesystem-owned block identity fixture
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <kern/block-identity.h>
#include <kern/disk.h>
#include <kern/fat.h>
#include <kern/fs.h>
#include <kern/mount.h>
#include <kern/partition.h>
#include <kern/swap.h>

#include "ufs1-disk.h"
#include "ufs1-endian.h"
#include "ufs2-disk.h"
#include "ufs2-endian.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define TEST_METADATA_BYTES (UFS2_SBLOCK_OFFSET + UFS2_SBLOCK_SIZE)
#define TEST_DISK_MAX 8U
#define TEST_PARTITION_MAX 4U

int fat_identify(struct disk *, struct block_identity *);
int ufs1_identify(struct disk *, struct block_identity *);
int ufs2_identify(struct disk *, struct block_identity *);

struct test_disk_state {
	uint8_t metadata[TEST_METADATA_BYTES];
	unsigned reads;
	int read_error;
};

static unsigned checks;
static unsigned callback_calls;
static unsigned nodev_calls;
static unsigned releases;
static struct disk *test_disks[TEST_DISK_MAX];
static unsigned test_disk_count;
static struct partition test_partitions[TEST_PARTITION_MAX];
static unsigned test_partition_count;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr, "KA-T030/031: failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                    \
			exit(1);                                             \
		}                                                            \
	} while (0)

void *
kern_malloc(size_t size)
{
	return malloc(size);
}

void *
kern_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	(void)mutex;
	(void)rank;
	(void)name;
	return 0;
}

void
waitq_init(struct wait_queue *queue, const char *name)
{
	(void)queue;
	(void)name;
}

int
backing_mutation_begin_disk(struct disk *disk, uint64_t first,
	uint64_t count, const struct backing_claim *claim,
	struct backing_mutation_guard *guard)
{
	(void)disk;
	(void)first;
	(void)count;
	(void)claim;
	(void)guard;
	return 0;
}

void
backing_mutation_end(struct backing_mutation_guard *guard)
{
	(void)guard;
}

int
backing_claim_check_mount(struct disk *disk, unsigned flags)
{
	(void)disk;
	(void)flags;
	return 0;
}

void
inode_ref(struct inode *inode)
{
	(void)inode;
}

void
inode_release(struct inode *inode)
{
	(void)inode;
}

int
disk_open(struct disk *disk)
{
	(void)disk;
	return 0;
}

void
disk_close(struct disk *disk)
{
	(void)disk;
}

int
disk_read_direct(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	struct test_disk_state *state;
	uint64_t offset;
	size_t length;

	if (disk == NULL || data == NULL || count == 0U ||
	    block >= disk->d_block_count || count > disk->d_block_count - block ||
	    disk->d_block_size == 0U)
		return EIO;
	state = disk->d_data;
	if (state == NULL)
		return EIO;
	state->reads++;
	if (state->read_error != 0)
		return state->read_error;
	offset = block * disk->d_block_size;
	length = (size_t)count * disk->d_block_size;
	if (offset >= sizeof(state->metadata)) {
		memset(data, 0, length);
		return 0;
	}
	if (length > sizeof(state->metadata) - (size_t)offset) {
		size_t available = sizeof(state->metadata) - (size_t)offset;

		memcpy(data, state->metadata + offset, available);
		memset((uint8_t *)data + available, 0, length - available);
	} else {
		memcpy(data, state->metadata + offset, length);
	}
	return 0;
}

int
disk_read(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	return disk_read_direct(disk, block, count, data);
}

int
disk_write_filesystem(struct disk *disk, uint64_t block, uint32_t count,
	const void *data)
{
	(void)disk;
	(void)block;
	(void)count;
	(void)data;
	return EROFS;
}

unsigned
disk_count(void)
{
	return test_disk_count;
}

struct disk *
disk_at(unsigned index)
{
	return index < test_disk_count ? test_disks[index] : NULL;
}

struct disk *
disk_find(const char *name)
{
	unsigned index;

	for (index = 0; index < test_disk_count; index++)
		if (strcmp(test_disks[index]->d_name, name) == 0)
			return test_disks[index];
	return NULL;
}

void
disk_release(struct disk *disk)
{
	if (disk != NULL)
		releases++;
}

unsigned
partition_count(void)
{
	return test_partition_count;
}

const struct partition *
partition_at(unsigned index)
{
	return index < test_partition_count ? &test_partitions[index] : NULL;
}

static int
dummy_mount(struct mount *mountp)
{
	(void)mountp;
	return 0;
}

enum callback_mode {
	CALLBACK_MISMATCH,
	CALLBACK_HARD_ERROR,
	CALLBACK_ONE_MATCH,
	CALLBACK_MULTIPLE_MATCHES,
	CALLBACK_FORBIDDEN_PARTITION,
	CALLBACK_NONTERMINATED_TYPE,
	CALLBACK_NONTERMINATED_UUID,
	CALLBACK_NONTERMINATED_LABEL,
	CALLBACK_UNFLAGGED_OUTPUT,
};

static enum callback_mode callback_mode;

static void
callback_identity(struct block_identity *identity, const char *type)
{
	memset(identity, 0, sizeof(*identity));
	strcpy(identity->type, type);
	strcpy(identity->uuid, "A1B2C3D4");
	strcpy(identity->label, "synthetic");
	identity->flags = ZEDBSD_BLKID_TYPE | ZEDBSD_BLKID_UUID |
	    ZEDBSD_BLKID_LABEL;
}

static int
nodev_identify(struct disk *disk, struct block_identity *identity)
{
	(void)disk;
	(void)identity;
	nodev_calls++;
	return EIO;
}

static int
callback_a(struct disk *disk, struct block_identity *identity)
{
	(void)disk;
	callback_calls++;
	if (callback_mode == CALLBACK_HARD_ERROR)
		return EIO;
	if (callback_mode == CALLBACK_ONE_MATCH ||
	    callback_mode == CALLBACK_MULTIPLE_MATCHES) {
		callback_identity(identity, "synthetic-a");
		return 0;
	}
	if (callback_mode == CALLBACK_FORBIDDEN_PARTITION) {
		callback_identity(identity, "forbidden");
		strcpy(identity->partuuid, "not-owned-by-filesystem");
		identity->flags |= ZEDBSD_BLKID_PARTUUID;
		return 0;
	}
	if (callback_mode == CALLBACK_NONTERMINATED_TYPE) {
		memset(identity, 0, sizeof(*identity));
		memset(identity->type, 'T', sizeof(identity->type));
		identity->flags = ZEDBSD_BLKID_TYPE;
		return 0;
	}
	if (callback_mode == CALLBACK_NONTERMINATED_UUID) {
		memset(identity, 0, sizeof(*identity));
		memset(identity->uuid, 'U', sizeof(identity->uuid));
		identity->flags = ZEDBSD_BLKID_UUID;
		return 0;
	}
	if (callback_mode == CALLBACK_NONTERMINATED_LABEL) {
		memset(identity, 0, sizeof(*identity));
		memset(identity->label, 'L', sizeof(identity->label));
		identity->flags = ZEDBSD_BLKID_LABEL;
		return 0;
	}
	if (callback_mode == CALLBACK_UNFLAGGED_OUTPUT) {
		memset(identity, 0, sizeof(*identity));
		identity->type[0] = 'x';
		return 0;
	}
	return EOPNOTSUPP;
}

static int
callback_b(struct disk *disk, struct block_identity *identity)
{
	(void)disk;
	callback_calls++;
	if (callback_mode == CALLBACK_MULTIPLE_MATCHES) {
		callback_identity(identity, "synthetic-b");
		return 0;
	}
	return EOPNOTSUPP;
}

static const struct filesystem_type nodev_type = {
	.fs_name = "nodev-test",
	.fs_flags = FILESYSTEM_NODEV,
	.identify = nodev_identify,
	.mount = dummy_mount,
};

static const struct filesystem_type null_type = {
	.fs_name = "null-test",
	.mount = dummy_mount,
};

static const struct filesystem_type callback_a_type = {
	.fs_name = "callback-a",
	.identify = callback_a,
	.mount = dummy_mount,
};

static const struct filesystem_type callback_b_type = {
	.fs_name = "callback-b",
	.identify = callback_b,
	.mount = dummy_mount,
};

static const struct filesystem_type fat_type = {
	.fs_name = "fat-identity-test",
	.identify = fat_identify,
	.mount = dummy_mount,
};

static const struct filesystem_type ufs1_type = {
	.fs_name = "ufs1-identity-test",
	.identify = ufs1_identify,
	.mount = dummy_mount,
};

static const struct filesystem_type ufs2_type = {
	.fs_name = "ufs2-identity-test",
	.identify = ufs2_identify,
	.mount = dummy_mount,
};

static void
disk_initialize(struct disk *disk, struct test_disk_state *state,
	const char *name, uint64_t blocks)
{
	memset(disk, 0, sizeof(*disk));
	memset(state, 0, sizeof(*state));
	strncpy(disk->d_name, name, sizeof(disk->d_name) - 1U);
	disk->d_block_size = 512U;
	disk->d_block_count = blocks;
	disk->d_data = state;
}

static void
put16(uint8_t *pointer, uint16_t value)
{
	pointer[0] = (uint8_t)value;
	pointer[1] = (uint8_t)(value >> 8);
}

static void
put32(uint8_t *pointer, uint32_t value)
{
	put16(pointer, (uint16_t)value);
	put16(pointer + 2, (uint16_t)(value >> 16));
}

static void
put64(uint8_t *pointer, uint64_t value)
{
	put32(pointer, (uint32_t)value);
	put32(pointer + 4, (uint32_t)(value >> 32));
}

static void
build_fat(struct disk *disk, struct test_disk_state *state,
	enum bootfat_type type)
{
	uint8_t *boot = state->metadata;
	uint32_t total;
	uint32_t fat_sectors;
	unsigned serial_offset;
	unsigned label_offset;

	if (type == ZEDBSD_FAT12) {
		total = 3000U;
		fat_sectors = 9U;
	} else if (type == ZEDBSD_FAT16) {
		total = 60000U;
		fat_sectors = 250U;
	} else {
		total = 70000U;
		fat_sectors = 600U;
	}
	disk->d_block_count = total;
	put16(boot + 11, 512U);
	boot[13] = 1U;
	put16(boot + 14, type == ZEDBSD_FAT32 ? 32U : 1U);
	boot[16] = 2U;
	put16(boot + 17, type == ZEDBSD_FAT32 ? 0U : 512U);
	if (total <= UINT16_MAX)
		put16(boot + 19, (uint16_t)total);
	else
		put32(boot + 32, total);
	if (type == ZEDBSD_FAT32) {
		put32(boot + 36, fat_sectors);
		put32(boot + 44, 2U);
		serial_offset = 67U;
		label_offset = 71U;
	} else {
		put16(boot + 22, (uint16_t)fat_sectors);
		serial_offset = 39U;
		label_offset = 43U;
	}
	boot[serial_offset - 1U] = 0x29U;
	put32(boot + serial_offset, UINT32_C(0x1234abcd));
	memcpy(boot + label_offset, "IDENTITY   ", 11U);
	boot[510] = 0x55U;
	boot[511] = 0xaaU;
}

static void
build_ufs1(struct disk *disk, struct test_disk_state *state, int swapped)
{
	uint8_t *super = state->metadata + UFS1_SBLOCK_OFFSET;

	disk->d_block_count = 16384U;
	ufs1_put32(super, UFS1_FS_SBLKNO, 8U, swapped);
	ufs1_put32(super, UFS1_FS_CBLKNO, 16U, swapped);
	ufs1_put32(super, UFS1_FS_IBLKNO, 24U, swapped);
	ufs1_put32(super, UFS1_FS_DBLKNO, 40U, swapped);
	ufs1_put32(super, UFS1_FS_OLD_SIZE, 4096U, swapped);
	ufs1_put32(super, UFS1_FS_OLD_DSIZE, 3000U, swapped);
	ufs1_put32(super, UFS1_FS_NCG, 1U, swapped);
	ufs1_put32(super, UFS1_FS_BSIZE, 8192U, swapped);
	ufs1_put32(super, UFS1_FS_FSIZE, 1024U, swapped);
	ufs1_put32(super, UFS1_FS_FRAG, 8U, swapped);
	ufs1_put32(super, UFS1_FS_BSHIFT, 13U, swapped);
	ufs1_put32(super, UFS1_FS_FSHIFT, 10U, swapped);
	ufs1_put32(super, UFS1_FS_FRAGSHIFT, 3U, swapped);
	ufs1_put32(super, UFS1_FS_FSBTODB, 1U, swapped);
	ufs1_put32(super, UFS1_FS_SBSIZE, UFS1_FS_STRUCT_SIZE, swapped);
	ufs1_put32(super, UFS1_FS_NINDIR, 2048U, swapped);
	ufs1_put32(super, UFS1_FS_INOPB, 64U, swapped);
	ufs1_put32(super, UFS1_FS_CGSIZE, 512U, swapped);
	ufs1_put32(super, UFS1_FS_IPG, 64U, swapped);
	ufs1_put32(super, UFS1_FS_FPG, 4096U, swapped);
	ufs1_put32(super, UFS1_FS_MAXSYMLINKLEN, 60U, swapped);
	ufs1_put32(super, UFS1_FS_INODEFMT, UFS1_44INODEFMT, swapped);
	ufs1_put64(super, UFS1_FS_MAXFILESIZE, UINT64_C(0x7fffffff), swapped);
	ufs1_put32(super, UFS1_FS_ID, UINT32_C(0x11223344), swapped);
	ufs1_put32(super, UFS1_FS_ID + 4U, UINT32_C(0xaabbccdd), swapped);
	memcpy(super + UFS1_FS_VOLNAME, "UFS ONE", 7U);
	ufs1_put32(super, UFS1_FS_MAGIC, UFS1_MAGIC, swapped);
}

static void
build_ufs2(struct disk *disk, struct test_disk_state *state, int swapped)
{
	uint8_t *super = state->metadata + UFS2_SBLOCK_OFFSET;

	disk->d_block_count = 16384U;
	ufs2_put32(super, UFS2_FS_SBLKNO, 64U, swapped);
	ufs2_put32(super, UFS2_FS_CBLKNO, 72U, swapped);
	ufs2_put32(super, UFS2_FS_IBLKNO, 80U, swapped);
	ufs2_put32(super, UFS2_FS_DBLKNO, 96U, swapped);
	ufs2_put32(super, UFS2_FS_NCG, 1U, swapped);
	ufs2_put32(super, UFS2_FS_BSIZE, 8192U, swapped);
	ufs2_put32(super, UFS2_FS_FSIZE, 1024U, swapped);
	ufs2_put32(super, UFS2_FS_FRAG, 8U, swapped);
	ufs2_put32(super, UFS2_FS_BSHIFT, 13U, swapped);
	ufs2_put32(super, UFS2_FS_FSHIFT, 10U, swapped);
	ufs2_put32(super, UFS2_FS_FRAGSHIFT, 3U, swapped);
	ufs2_put32(super, UFS2_FS_FSBTODB, 1U, swapped);
	ufs2_put32(super, UFS2_FS_SBSIZE, UFS2_FS_STRUCT_SIZE, swapped);
	ufs2_put32(super, UFS2_FS_NINDIR, 1024U, swapped);
	ufs2_put32(super, UFS2_FS_INOPB, 32U, swapped);
	ufs2_put32(super, UFS2_FS_CGSIZE, 512U, swapped);
	ufs2_put32(super, UFS2_FS_IPG, 32U, swapped);
	ufs2_put32(super, UFS2_FS_FPG, 8192U, swapped);
	ufs2_put64(super, UFS2_FS_SBLOCKLOC, UFS2_SBLOCK_OFFSET, swapped);
	ufs2_put64(super, UFS2_FS_SIZE, 8192U, swapped);
	ufs2_put64(super, UFS2_FS_DSIZE, 7000U, swapped);
	ufs2_put32(super, UFS2_FS_MAXSYMLINKLEN, 120U, swapped);
	ufs2_put64(super, UFS2_FS_MAXFILESIZE,
	    UINT64_C(0x7fffffffffff), swapped);
	ufs2_put32(super, UFS2_FS_ID, UINT32_C(0x55667788), swapped);
	ufs2_put32(super, UFS2_FS_ID + 4U, UINT32_C(0x99aabbcc), swapped);
	memcpy(super + UFS2_FS_VOLNAME, "UFS TWO", 7U);
	ufs2_put32(super, UFS2_FS_MAGIC, UFS2_MAGIC, swapped);
}

static void
build_swap(struct disk *disk, struct test_disk_state *state)
{
	uint8_t *header = state->metadata;
	uint64_t bytes = 64ULL * 1024U * 1024U;
	unsigned index;

	disk->d_block_count = bytes / disk->d_block_size;
	memcpy(header, "ZEDSWAP2", 8U);
	put16(header + 8U, 2U);
	put16(header + 10U, ZEDBSD_SWAP_HEADER_SIZE);
	put32(header + 12U, SWAP_PAGE_SIZE);
	put64(header + 16U, bytes);
	put64(header + 24U, bytes / SWAP_PAGE_SIZE - 1U);
	for (index = 0; index < ZEDBSD_SWAP_V2_UUID_SIZE; index++)
		header[32U + index] = (uint8_t)(index + 1U);
	memcpy(header + 40U, "swap-data", 10U);
	put32(header + 60U, swap_header_checksum(header));
}

static void
expect_identity(struct disk *disk, const char *type, const char *uuid,
	const char *label)
{
	struct block_identity identity;

	CHECK(block_identity_get(disk, &identity) == 0);
	CHECK((identity.flags & ZEDBSD_BLKID_TYPE) != 0U);
	CHECK(strcmp(identity.type, type) == 0);
	CHECK(strcmp(identity.uuid, uuid) == 0);
	CHECK(strcmp(identity.label, label) == 0);
}

static void
test_dispatcher(void)
{
	struct disk disk;
	struct test_disk_state state;
	struct block_identity identity;
	unsigned before;

	disk_initialize(&disk, &state, "blank", 256U);
	CHECK(filesystem_identify(NULL, &identity) == EINVAL);
	memset(&identity, 0xa5, sizeof(identity));
	CHECK(filesystem_identify(&disk, NULL) == EINVAL);

	callback_mode = CALLBACK_MISMATCH;
	CHECK(filesystem_identify(&disk, &identity) == EOPNOTSUPP);
	CHECK(identity.flags == 0U);
	CHECK(nodev_calls == 0U);

	callback_mode = CALLBACK_HARD_ERROR;
	CHECK(filesystem_identify(&disk, &identity) == EIO);
	CHECK(identity.flags == 0U);

	callback_mode = CALLBACK_ONE_MATCH;
	CHECK(filesystem_identify(&disk, &identity) == 0);
	CHECK(strcmp(identity.type, "synthetic-a") == 0);

	callback_mode = CALLBACK_MULTIPLE_MATCHES;
	memset(&identity, 0xa5, sizeof(identity));
	CHECK(filesystem_identify(&disk, &identity) == EEXIST);
	CHECK(identity.flags == 0U);

	callback_mode = CALLBACK_FORBIDDEN_PARTITION;
	CHECK(filesystem_identify(&disk, &identity) == EINVAL);
	callback_mode = CALLBACK_NONTERMINATED_TYPE;
	CHECK(filesystem_identify(&disk, &identity) == EINVAL);
	callback_mode = CALLBACK_NONTERMINATED_UUID;
	CHECK(filesystem_identify(&disk, &identity) == EINVAL);
	callback_mode = CALLBACK_NONTERMINATED_LABEL;
	CHECK(filesystem_identify(&disk, &identity) == EINVAL);
	callback_mode = CALLBACK_UNFLAGGED_OUTPUT;
	CHECK(filesystem_identify(&disk, &identity) == EINVAL);
	CHECK(nodev_calls == 0U);

	before = callback_calls;
	callback_mode = CALLBACK_MISMATCH;
	CHECK(filesystem_identify(&disk, &identity) == EOPNOTSUPP);
	CHECK(callback_calls == before + 2U);
}

static void
test_filesystem_formats(void)
{
	struct disk disk;
	struct test_disk_state state;
	struct block_identity identity;

	disk_initialize(&disk, &state, "fat12", 1U);
	build_fat(&disk, &state, ZEDBSD_FAT12);
	expect_identity(&disk, "vfat", "1234-ABCD", "IDENTITY");

	disk_initialize(&disk, &state, "fat16", 1U);
	build_fat(&disk, &state, ZEDBSD_FAT16);
	expect_identity(&disk, "vfat", "1234-ABCD", "IDENTITY");

	disk_initialize(&disk, &state, "fat32", 1U);
	build_fat(&disk, &state, ZEDBSD_FAT32);
	expect_identity(&disk, "vfat", "1234-ABCD", "IDENTITY");

	disk_initialize(&disk, &state, "ufs1le", 1U);
	build_ufs1(&disk, &state, 0);
	expect_identity(&disk, "ufs1", "11223344AABBCCDD", "UFS ONE");

	disk_initialize(&disk, &state, "ufs1be", 1U);
	build_ufs1(&disk, &state, 1);
	expect_identity(&disk, "ufs1", "11223344AABBCCDD", "UFS ONE");

	disk_initialize(&disk, &state, "ufs2le", 1U);
	build_ufs2(&disk, &state, 0);
	expect_identity(&disk, "ufs2", "5566778899AABBCC", "UFS TWO");

	disk_initialize(&disk, &state, "ufs2be", 1U);
	build_ufs2(&disk, &state, 1);
	expect_identity(&disk, "ufs2", "5566778899AABBCC", "UFS TWO");

	disk_initialize(&disk, &state, "blank", 256U);
	CHECK(block_identity_get(&disk, &identity) == ENOENT);

	disk_initialize(&disk, &state, "io-error", 16384U);
	state.read_error = EIO;
	CHECK(block_identity_get(&disk, &identity) == EIO);
	CHECK(disk.d_identity_valid == 0U);
	state.read_error = 0;
	CHECK(block_identity_get(&disk, &identity) == ENOENT);
	CHECK(state.reads > 1U);

	disk_initialize(&disk, &state, "truncated-fat", 1U);
	build_fat(&disk, &state, ZEDBSD_FAT16);
	disk.d_block_count = 1U;
	CHECK(block_identity_get(&disk, &identity) != 0);

	/* Keep the crash-safety regression after other bounded-error coverage. */
	disk_initialize(&disk, &state, "bad-ufs", 16384U);
	ufs1_put32(state.metadata + UFS1_SBLOCK_OFFSET, UFS1_FS_MAGIC,
	    UFS1_MAGIC, 0);
	CHECK(block_identity_get(&disk, &identity) == EINVAL);
}

static void
test_partition_selectors(void)
{
	struct disk first;
	struct disk second;
	struct test_disk_state first_state;
	struct test_disk_state second_state;
	struct disk *result;
	unsigned calls_before;

	disk_initialize(&first, &first_state, "part0", 256U);
	disk_initialize(&second, &second_state, "part1", 256U);
	first.d_flags |= DISK_PARTITION;
	second.d_flags |= DISK_PARTITION;
	test_disks[0] = &first;
	test_disk_count = 1U;
	memset(test_partitions, 0, sizeof(test_partitions));
	test_partitions[0].p_disk = &first;
	test_partitions[0].p_flags = PARTITION_HAS_UUID | PARTITION_HAS_LABEL;
	strcpy(test_partitions[0].p_uuid, "0011-AABB");
	strcpy(test_partitions[0].p_label, "System Partition");
	test_partition_count = 1U;
	callback_mode = CALLBACK_HARD_ERROR;
	calls_before = callback_calls;
	CHECK(block_identity_resolve("PARTUUID=0011-aabb", &result) == 0);
	CHECK(result == &first);
	CHECK(callback_calls == calls_before);
	disk_release(result);
	CHECK(block_identity_resolve("PARTLABEL=system partition", &result) ==
	    0);
	CHECK(result == &first);
	CHECK(callback_calls == calls_before);
	disk_release(result);

	test_disks[1] = &second;
	test_disk_count = 2U;
	test_partitions[1].p_disk = &second;
	test_partitions[1].p_flags = PARTITION_HAS_UUID;
	strcpy(test_partitions[1].p_uuid, "0011-AABB");
	test_partition_count = 2U;
	CHECK(block_identity_resolve("PARTUUID=0011-AABB", &result) == EEXIST);
	CHECK(result == NULL);
	CHECK(callback_calls == calls_before);
	CHECK(releases >= 4U);

	test_disk_count = 0U;
	test_partition_count = 0U;
}

static void
test_filesystem_selectors(void)
{
	struct disk fat;
	struct disk ufs;
	struct disk duplicate;
	struct disk hard_error;
	struct disk target;
	struct test_disk_state fat_state;
	struct test_disk_state ufs_state;
	struct test_disk_state duplicate_state;
	struct test_disk_state hard_error_state;
	struct test_disk_state target_state;
	struct disk *result;

	callback_mode = CALLBACK_MISMATCH;
	test_partition_count = 0U;
	disk_initialize(&fat, &fat_state, "selector-fat", 1U);
	build_fat(&fat, &fat_state, ZEDBSD_FAT16);
	disk_initialize(&ufs, &ufs_state, "selector-ufs", 1U);
	build_ufs1(&ufs, &ufs_state, 0);
	test_disks[0] = &fat;
	test_disks[1] = &ufs;
	test_disk_count = 2U;

	CHECK(block_identity_resolve("UUID=1234-abcd", &result) == 0);
	CHECK(result == &fat);
	disk_release(result);
	CHECK(block_identity_resolve("LABEL=identity", &result) == 0);
	CHECK(result == &fat);
	disk_release(result);
	CHECK(block_identity_resolve("UUID=11223344aabbccdd", &result) == 0);
	CHECK(result == &ufs);
	disk_release(result);
	CHECK(block_identity_resolve("LABEL=ufs one", &result) == 0);
	CHECK(result == &ufs);
	disk_release(result);

	disk_initialize(&duplicate, &duplicate_state, "selector-duplicate", 1U);
	build_fat(&duplicate, &duplicate_state, ZEDBSD_FAT12);
	test_disks[2] = &duplicate;
	test_disk_count = 3U;
	CHECK(block_identity_resolve("UUID=1234-ABCD", &result) == EEXIST);
	CHECK(result == NULL);
	CHECK(block_identity_resolve("LABEL=IDENTITY", &result) == EEXIST);
	CHECK(result == NULL);

	disk_initialize(&hard_error, &hard_error_state, "selector-error", 16384U);
	hard_error_state.read_error = EIO;
	disk_initialize(&target, &target_state, "selector-target", 1U);
	build_ufs2(&target, &target_state, 1);
	test_disks[0] = &hard_error;
	test_disks[1] = &target;
	test_disk_count = 2U;
	CHECK(block_identity_resolve("UUID=5566778899aabbcc", &result) == 0);
	CHECK(result == &target);
	disk_release(result);
	CHECK(hard_error_state.reads != 0U);
	CHECK(block_identity_resolve("LABEL=ufs two", &result) == 0);
	CHECK(result == &target);
	disk_release(result);

	test_disk_count = 0U;
}

static void
test_swap_hybrid_and_cache(void)
{
	struct disk swap_disk;
	struct disk hybrid;
	struct disk cached;
	struct disk replacement;
	struct test_disk_state swap_state;
	struct test_disk_state hybrid_state;
	struct test_disk_state cache_state;
	struct block_identity identity;
	unsigned reads;

	callback_mode = CALLBACK_MISMATCH;
	disk_initialize(&swap_disk, &swap_state, "swap", 1U);
	build_swap(&swap_disk, &swap_state);
	expect_identity(&swap_disk, "swap", "0102030405060708", "swap-data");

	disk_initialize(&hybrid, &hybrid_state, "hybrid", 1U);
	build_ufs1(&hybrid, &hybrid_state, 0);
	build_swap(&hybrid, &hybrid_state);
	CHECK(block_identity_get(&hybrid, &identity) == EEXIST);
	CHECK(hybrid.d_identity_valid == 0U);

	disk_initialize(&cached, &cache_state, "cached", 1U);
	build_fat(&cached, &cache_state, ZEDBSD_FAT16);
	CHECK(block_identity_get(&cached, &identity) == 0);
	CHECK(strcmp(identity.uuid, "1234-ABCD") == 0);
	reads = cache_state.reads;
	cache_state.read_error = EIO;
	CHECK(block_identity_get(&cached, &identity) == 0);
	CHECK(cache_state.reads == reads);
	CHECK(strcmp(identity.uuid, "1234-ABCD") == 0);

	memset(&replacement, 0, sizeof(replacement));
	strcpy(replacement.d_name, "cached-new");
	replacement.d_block_size = cached.d_block_size;
	replacement.d_block_count = cached.d_block_count;
	replacement.d_data = &cache_state;
	cache_state.read_error = 0;
	put32(cache_state.metadata + 39U, UINT32_C(0x55667788));
	CHECK(block_identity_get(&replacement, &identity) == 0);
	CHECK(cache_state.reads > reads);
	CHECK(strcmp(identity.uuid, "5566-7788") == 0);
}

int
main(void)
{
	CHECK(filesystem_register(&nodev_type) == 0);
	CHECK(filesystem_register(&null_type) == 0);
	CHECK(filesystem_register(&callback_a_type) == 0);
	CHECK(filesystem_register(&callback_b_type) == 0);
	CHECK(filesystem_register(&fat_type) == 0);
	CHECK(filesystem_register(&ufs1_type) == 0);
	CHECK(filesystem_register(&ufs2_type) == 0);

	test_dispatcher();
	test_filesystem_formats();
	test_partition_selectors();
	test_filesystem_selectors();
	test_swap_hybrid_and_cache();
	printf("KA-T030/KA-T031: PASS (%u checks)\n", checks);
	return 0;
}
