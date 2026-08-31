/*
 * WS018 KA-T100/KA-T101: native FAT VFS host fixture
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <kern/fat.h>
#include <kern/namecache.h>
#include <kern/namei.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define SECTOR_SIZE 512U
#define HOST_INODE_MAX 512U
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

struct memory_image {
	struct disk disk;
	uint8_t *bytes;
	uint32_t sectors;
	uint32_t reserved;
	uint32_t fat_sectors;
	uint32_t root_sectors;
	uint32_t root_start;
	uint32_t data_start;
	uint32_t cluster_count;
	uint32_t root_cluster;
	uint16_t logical_sector_size;
	uint16_t sectors_per_cluster;
	uint8_t fat_copies;
	enum bootfat_type type;
	unsigned reads;
	unsigned direct_reads;
	unsigned writes;
	unsigned write_attempts;
	unsigned syncs;
	unsigned fail_reads;
	unsigned fail_writes;
	unsigned fail_write_attempt;
	unsigned fail_write_attempt2;
	unsigned arm_read_after_writes;
	unsigned fail_syncs;
};

struct extent_capture {
	uint64_t file_block[8];
	uint64_t disk_block[8];
	uint32_t count[8];
	unsigned used;
	int reject;
};

static struct inode *host_inode_cache[HOST_INODE_MAX];
static unsigned checks;
static unsigned inode_allocations;
static unsigned inode_free_attempts;
static unsigned inode_destructions;
static unsigned namecache_removes;
static unsigned namecache_purges;
static unsigned fail_kern_allocations;
static enum bootfat_type current_type;
static const char *current_stage = "startup";
static unsigned current_fault_ordinal;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr,                                        \
			    "KA-T100/101 FAT%u (%s, fault %u): failed at "     \
			    "%s:%d: %s\n",                                   \
			    (unsigned)current_type, current_stage,              \
			    current_fault_ordinal, __FILE__, __LINE__,           \
			    #expression);                                      \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

#define CHECK_ERROR(expression, wanted)                                    \
	do {                                                                 \
		int check_result_ = (expression);                             \
		int check_wanted_ = (wanted);                                 \
		checks++;                                                    \
		if (check_result_ != check_wanted_) {                         \
			fprintf(stderr,                                        \
			    "KA-T100/101 FAT%u (%s, fault %u): failed at %s:%d: " \
			    "%s returned %d, wanted %d\n",                   \
			    (unsigned)current_type, current_stage,              \
			    current_fault_ordinal, __FILE__, __LINE__,           \
			    #expression, check_result_, check_wanted_);         \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

/* FAT's bounded rollback journal uses the ordinary kernel allocator. */

void *
kern_malloc(size_t size)
{
	if (fail_kern_allocations != 0U) {
		fail_kern_allocations--;
		return NULL;
	}
	return malloc(size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

/* Host lock boundary: every scenario is deliberately single-threaded. */

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
mutex_lock(struct mutex *mutex)
{
	(void)mutex;
}

void
mutex_unlock(struct mutex *mutex)
{
	(void)mutex;
}

/* Host namecache boundary.  The FAT driver owns every invalidation choice. */

void
namecache_remove(struct inode *parent, const struct componentname *name)
{
	CHECK(parent != NULL);
	CHECK(name != NULL);
	namecache_removes++;
}

void
namecache_purge_inode(struct inode *inode)
{
	CHECK(inode != NULL);
	namecache_purges++;
}

/*
 * Host inode-cache boundary.  It retains the kernel contract of one cache
 * reference plus one returned reference and destroys dead inodes only after
 * the final file/caller reference is released.
 */

static int
host_inode_index(const struct inode *inode)
{
	unsigned i;

	for (i = 0; i < HOST_INODE_MAX; i++)
		if (host_inode_cache[i] == inode)
			return (int)i;
	return -1;
}

struct inode *
inode_alloc(struct mount *mountp)
{
	struct inode *inode;
	unsigned slot;

	if (mountp == NULL || mountp->m_type == NULL ||
	    mountp->m_type->alloc_inode == NULL)
		return NULL;
	for (slot = 0; slot < HOST_INODE_MAX; slot++)
		if (host_inode_cache[slot] == NULL)
			break;
	if (slot == HOST_INODE_MAX)
		return NULL;
	inode = mountp->m_type->alloc_inode(mountp);
	if (inode == NULL)
		return NULL;
	memset(inode, 0, sizeof(*inode));
	inode->i_mount = mountp;
	inode->i_dirseq = 1;
	refcount_init(&inode->i_refs, 2U);
	host_inode_cache[slot] = inode;
	inode_allocations++;
	return inode;
}

void
inode_free(struct inode *inode)
{
	struct mount *mountp;
	int index;

	inode_free_attempts++;
	CHECK(inode != NULL);
	CHECK(refcount_load(&inode->i_refs) == 1U);
	index = host_inode_index(inode);
	CHECK(index >= 0);
	host_inode_cache[index] = NULL;
	CHECK(refcount_put(&inode->i_refs));
	mountp = inode->i_mount;
	if (inode->i_op != NULL && inode->i_op->reclaim != NULL)
		inode->i_op->reclaim(inode);
	if (mountp != NULL && mountp->m_type != NULL &&
	    mountp->m_type->free_inode != NULL)
		mountp->m_type->free_inode(inode);
	inode_destructions++;
}

int
inode_get(struct mount *mountp, ino_t ino, struct inode **result)
{
	unsigned i;

	if (mountp == NULL || result == NULL)
		return EINVAL;
	for (i = 0; i < HOST_INODE_MAX; i++) {
		struct inode *inode = host_inode_cache[i];

		if (inode != NULL && inode->i_mount == mountp &&
		    inode->i_ino == ino && (inode->i_flags & INODE_DEAD) == 0) {
			inode_ref(inode);
			*result = inode;
			return 0;
		}
	}
	return ENOENT;
}

void
inode_ref(struct inode *inode)
{
	if (inode != NULL)
		refcount_get(&inode->i_refs);
}

void
inode_release(struct inode *inode)
{
	unsigned remaining;

	if (inode == NULL)
		return;
	remaining = refcount_put_not_last(&inode->i_refs);
	if (remaining == 1U && (inode->i_flags & INODE_DEAD) != 0 &&
	    (inode->i_flags & (INODE_DIRTY | INODE_ROOT)) == 0)
		inode_free(inode);
}

int
inode_creation_prepare(struct inode *parent, struct inode *child,
	const struct inode_creation_request *request)
{
	(void)parent;
	if (child == NULL || request == NULL ||
	    request->origin == INODE_CREATION_INVALID ||
	    child->i_type != request->type)
		return EINVAL;
	child->i_mode = (child->i_mode & S_IFMT) | (request->mode & 07777U);
	child->i_uid = request->uid;
	child->i_gid = request->gid;
	child->i_rdev = request->rdev;
	child->i_special = request->special;
	return 0;
}

/* Host disk boundary with deterministic one-shot fault injection. */

static int
memory_transfer(struct disk *disk, uint64_t block, uint32_t count, void *data,
	int write)
{
	struct memory_image *image;
	size_t bytes;

	if (disk == NULL || data == NULL || count == 0)
		return EINVAL;
	image = disk->d_data;
	if (image == NULL || block >= image->sectors ||
	    count > image->sectors - block)
		return EIO;
	if (write && (disk->d_flags & DISK_READ_ONLY) != 0)
		return EROFS;
	if (write) {
		image->write_attempts++;
		if (image->fail_write_attempt == image->write_attempts) {
			image->fail_write_attempt = 0;
			return EIO;
		}
		if (image->fail_write_attempt2 == image->write_attempts) {
			image->fail_write_attempt2 = 0;
			return EIO;
		}
		if (image->fail_writes != 0) {
			image->fail_writes--;
			return EIO;
		}
	}
	if (!write && image->fail_reads != 0) {
		image->fail_reads--;
		return EIO;
	}
	bytes = (size_t)count * SECTOR_SIZE;
	if (write) {
		memcpy(image->bytes + (size_t)block * SECTOR_SIZE, data, bytes);
		image->writes++;
		if (image->arm_read_after_writes != 0U &&
		    image->writes >= image->arm_read_after_writes) {
			image->arm_read_after_writes = 0U;
			image->fail_reads++;
		}
	} else {
		memcpy(data, image->bytes + (size_t)block * SECTOR_SIZE, bytes);
		image->reads++;
	}
	return 0;
}

int
disk_read(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	return memory_transfer(disk, block, count, data, 0);
}

int
disk_read_direct(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	struct memory_image *image = disk != NULL ? disk->d_data : NULL;
	int error = memory_transfer(disk, block, count, data, 0);

	if (error == 0 && image != NULL)
		image->direct_reads++;
	return error;
}

int
disk_write_filesystem(struct disk *disk, uint64_t block, uint32_t count,
	const void *data)
{
	return memory_transfer(disk, block, count, (void *)data, 1);
}

int
disk_sync(struct disk *disk)
{
	struct memory_image *image;

	if (disk == NULL || disk->d_data == NULL)
		return EINVAL;
	image = disk->d_data;
	image->syncs++;
	if (image->fail_syncs != 0) {
		image->fail_syncs--;
		return EIO;
	}
	return 0;
}

/* Deterministic FAT image construction; no production decoder is copied. */

static void
put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
put32(uint8_t *bytes, uint32_t value)
{
	put16(bytes, (uint16_t)value);
	put16(bytes + 2, (uint16_t)(value >> 16));
}

static uint8_t *
image_sector(struct memory_image *image, uint32_t lba)
{
	CHECK(image != NULL);
	CHECK(lba < image->sectors);
	return image->bytes + (size_t)lba * SECTOR_SIZE;
}

static uint32_t
cluster_lba(const struct memory_image *image, uint32_t cluster)
{
	CHECK(cluster >= 2U);
	CHECK(cluster < image->cluster_count + 2U);
	return image->data_start +
	    (cluster - 2U) * image->sectors_per_cluster;
}

static uint32_t
fat_end_of_chain(enum bootfat_type type)
{
	return type == ZEDBSD_FAT12 ? 0x0fffU :
	       type == ZEDBSD_FAT16 ? 0xffffU : 0x0fffffffU;
}

static void
set_one_fat_entry(struct memory_image *image, unsigned copy,
	uint32_t cluster, uint32_t value)
{
	uint8_t *fat = image_sector(image,
	    image->reserved + copy * image->fat_sectors);
	uint32_t offset;

	if (image->type == ZEDBSD_FAT12) {
		uint16_t packed;

		offset = cluster + cluster / 2U;
		CHECK(offset + 1U < image->fat_sectors * SECTOR_SIZE);
		packed = (uint16_t)fat[offset] |
		    ((uint16_t)fat[offset + 1U] << 8);
		if ((cluster & 1U) != 0)
			packed = (uint16_t)((packed & 0x000fU) |
			    ((value & 0x0fffU) << 4));
		else
			packed = (uint16_t)((packed & 0xf000U) |
			    (value & 0x0fffU));
		fat[offset] = (uint8_t)packed;
		fat[offset + 1U] = (uint8_t)(packed >> 8);
		return;
	}
	offset = cluster * (image->type == ZEDBSD_FAT16 ? 2U : 4U);
	CHECK(offset + (image->type == ZEDBSD_FAT16 ? 1U : 3U) <
	    image->fat_sectors * SECTOR_SIZE);
	if (image->type == ZEDBSD_FAT16)
		put16(fat + offset, (uint16_t)value);
	else
		put32(fat + offset, value & 0x0fffffffU);
}

static void
set_fat_entry(struct memory_image *image, uint32_t cluster, uint32_t value)
{
	unsigned copy;

	for (copy = 0; copy < image->fat_copies; copy++)
		set_one_fat_entry(image, copy, cluster, value);
}

static uint32_t
get_fat_entry(const struct memory_image *image, unsigned copy,
	uint32_t cluster)
{
	const uint8_t *fat = image->bytes +
	    (size_t)(image->reserved + copy * image->fat_sectors) * SECTOR_SIZE;
	uint32_t offset;

	if (image->type == ZEDBSD_FAT12) {
		uint16_t packed;

		offset = cluster + cluster / 2U;
		packed = (uint16_t)fat[offset] |
		    ((uint16_t)fat[offset + 1U] << 8);
		return (cluster & 1U) != 0 ? packed >> 4 : packed & 0x0fffU;
	}
	offset = cluster * (image->type == ZEDBSD_FAT16 ? 2U : 4U);
	if (image->type == ZEDBSD_FAT16)
		return (uint32_t)fat[offset] | ((uint32_t)fat[offset + 1U] << 8);
	return ((uint32_t)fat[offset] | ((uint32_t)fat[offset + 1U] << 8) |
	    ((uint32_t)fat[offset + 2U] << 16) |
	    ((uint32_t)fat[offset + 3U] << 24)) & 0x0fffffffU;
}

static uint16_t
get16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t
raw_dirent_cluster(const struct memory_image *image, const uint8_t *entry)
{
	uint32_t cluster = get16(entry + 26U);

	if (image->type == ZEDBSD_FAT32)
		cluster |= (uint32_t)get16(entry + 20U) << 16;
	return cluster;
}

static uint32_t
raw_find_sfn_cluster(struct memory_image *image, uint32_t directory_cluster,
	const uint8_t sfn[11])
{
	uint32_t lba = directory_cluster == 0U ? image->root_start :
	    cluster_lba(image, directory_cluster);
	unsigned entries = (directory_cluster == 0U ? image->root_sectors :
	    image->sectors_per_cluster) * (SECTOR_SIZE / 32U);
	const uint8_t *directory = image_sector(image, lba);
	unsigned i;

	for (i = 0; i < entries; i++) {
		const uint8_t *entry = directory + i * 32U;

		if (entry[0] == 0U)
			break;
		if (entry[0] != 0xe5U && entry[11] != 0x0fU &&
		    memcmp(entry, sfn, 11U) == 0)
			return raw_dirent_cluster(image, entry);
	}
	CHECK(0);
	return 0;
}

static uint32_t
raw_dotdot_parent_cluster(struct memory_image *image,
	uint32_t directory_cluster)
{
	const uint8_t *directory = image_sector(image,
	    cluster_lba(image, directory_cluster));

	CHECK(directory[32U] == '.');
	CHECK(directory[33U] == '.');
	CHECK(directory[32U + 11U] == 0x10U);
	return raw_dirent_cluster(image, directory + 32U);
}

static void
check_fat_copies(const struct memory_image *image)
{
	unsigned copy;

	for (copy = 1; copy < image->fat_copies; copy++)
		CHECK(memcmp(image->bytes +
		    (size_t)image->reserved * SECTOR_SIZE,
		    image->bytes + (size_t)(image->reserved +
		    copy * image->fat_sectors) * SECTOR_SIZE,
		    (size_t)image->fat_sectors * SECTOR_SIZE) == 0);
}

static void
set_dirent(uint8_t raw[32], const uint8_t name[11], uint8_t attributes,
	uint32_t cluster, uint32_t size, enum bootfat_type type)
{
	memset(raw, 0, 32);
	memcpy(raw, name, 11);
	raw[11] = attributes;
	put16(raw + 26, (uint16_t)cluster);
	if (type == ZEDBSD_FAT32)
		put16(raw + 20, (uint16_t)(cluster >> 16));
	put32(raw + 28, size);
}

static uint8_t
lfn_checksum(const uint8_t sfn[11])
{
	uint8_t sum = 0;
	unsigned i;

	for (i = 0; i < 11; i++)
		sum = (uint8_t)(((sum & 1U) << 7) | (sum >> 1)) + sfn[i];
	return sum;
}

static void
set_lfn_entry(uint8_t raw[32], const char *name, unsigned ordinal,
	uint8_t checksum)
{
	static const uint8_t offsets[13] = {
	    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
	};
	unsigned length = (unsigned)strlen(name);
	unsigned total = (length + 12U) / 13U;
	unsigned i;

	memset(raw, 0xff, 32);
	raw[0] = (uint8_t)ordinal;
	if (ordinal == total)
		raw[0] |= 0x40U;
	raw[11] = 0x0fU;
	raw[12] = 0;
	raw[13] = checksum;
	raw[26] = raw[27] = 0;
	for (i = 0; i < 13; i++) {
		unsigned index = (ordinal - 1U) * 13U + i;
		uint16_t value = index < length ? (uint8_t)name[index] :
		    (index == length ? 0U : 0xffffU);

		put16(raw + offsets[i], value);
	}
}

static void
put_cluster_bytes(struct memory_image *image, uint32_t cluster,
	const void *input, size_t length)
{
	CHECK(length <= (size_t)image->sectors_per_cluster * SECTOR_SIZE);
	memcpy(image_sector(image, cluster_lba(image, cluster)), input, length);
}

static void
format_image(struct memory_image *image, enum bootfat_type type,
	unsigned sector_scale, unsigned fat_copies)
{
	static const uint8_t hello_sfn[11] = {
	    'H', 'E', 'L', 'L', 'O', ' ', ' ', ' ', 'T', 'X', 'T',
	};
	static const uint8_t long_sfn[11] = {
	    'L', 'O', 'N', 'G', 'F', 'I', '~', '1', 'T', 'X', 'T',
	};
	static const char long_name[] = "Long File Name.txt";
	static const char long_payload[] = "long payload";
	uint8_t hello[700];
	uint8_t *boot;
	uint8_t *root;
	uint32_t eoc = fat_end_of_chain(type);
	uint32_t cluster_bytes;
	unsigned i;

	memset(image, 0, sizeof(*image));
	CHECK(sector_scale == 1U || sector_scale == 2U);
	CHECK(fat_copies == 1U || fat_copies == 2U);
	image->type = type;
	image->logical_sector_size = (uint16_t)(SECTOR_SIZE * sector_scale);
	image->sectors_per_cluster = (uint16_t)sector_scale;
	image->fat_copies = (uint8_t)fat_copies;
	if (type == ZEDBSD_FAT12) {
		image->sectors = 128U;
		image->reserved = sector_scale;
		image->fat_sectors = sector_scale;
		image->root_sectors = 4U;
	} else if (type == ZEDBSD_FAT16) {
		image->sectors = 5000U;
		image->reserved = sector_scale;
		image->fat_sectors = 20U * sector_scale;
		image->root_sectors = 4U;
	} else {
		CHECK(sector_scale == 1U);
		image->sectors = 67000U;
		image->reserved = 32U;
		image->fat_sectors = 524U;
		image->root_cluster = 2U;
	}
	image->root_start = image->reserved +
	    image->fat_sectors * image->fat_copies;
	image->data_start = image->root_start + image->root_sectors;
	image->cluster_count =
	    (image->sectors - image->data_start) /
	    image->sectors_per_cluster;
	image->bytes = calloc((size_t)image->sectors, SECTOR_SIZE);
	CHECK(image->bytes != NULL);

	boot = image_sector(image, 0);
	boot[0] = 0xebU;
	boot[1] = 0x3cU;
	boot[2] = 0x90U;
	memcpy(boot + 3, "ZEDBSD  ", 8);
	put16(boot + 11, image->logical_sector_size);
	boot[13] = 1U;
	put16(boot + 14, (uint16_t)(image->reserved / sector_scale));
	boot[16] = image->fat_copies;
	put16(boot + 17, type == ZEDBSD_FAT32 ? 0U : 64U);
	put16(boot + 19, image->sectors / sector_scale <= UINT16_MAX ?
	    (uint16_t)(image->sectors / sector_scale) : 0U);
	boot[21] = 0xf8U;
	put16(boot + 22, type == ZEDBSD_FAT32 ? 0U :
	    (uint16_t)(image->fat_sectors / sector_scale));
	put16(boot + 24, 63U);
	put16(boot + 26, 255U);
	if (image->sectors / sector_scale > UINT16_MAX)
		put32(boot + 32, image->sectors / sector_scale);
	if (type == ZEDBSD_FAT32) {
		put32(boot + 36, image->fat_sectors);
		put32(boot + 44, image->root_cluster);
		put16(boot + 48, 1U);
	}
	boot[510] = 0x55U;
	boot[511] = 0xaaU;

	set_fat_entry(image, 0U,
	    type == ZEDBSD_FAT12 ? 0x0ff8U :
	    type == ZEDBSD_FAT16 ? 0xfff8U : 0x0ffffff8U);
	set_fat_entry(image, 1U, eoc);
	if (type == ZEDBSD_FAT32)
		set_fat_entry(image, image->root_cluster, eoc);
	cluster_bytes = (uint32_t)image->sectors_per_cluster * SECTOR_SIZE;
	if (sizeof(hello) > cluster_bytes) {
		set_fat_entry(image, 3U, 4U);
		set_fat_entry(image, 4U, eoc);
	} else {
		set_fat_entry(image, 3U, eoc);
	}

	root = type == ZEDBSD_FAT32 ?
	    image_sector(image, cluster_lba(image, image->root_cluster)) :
	    image_sector(image, image->root_start);
	set_dirent(root, hello_sfn, 0x20U, 3U, sizeof(hello), type);
	for (i = 0; i < ARRAY_COUNT(hello); i++)
		hello[i] = (uint8_t)('A' + i % 26U);
	if (sizeof(hello) <= cluster_bytes) {
		put_cluster_bytes(image, 3U, hello, sizeof(hello));
	} else {
		put_cluster_bytes(image, 3U, hello, cluster_bytes);
		put_cluster_bytes(image, 4U, hello + cluster_bytes,
		    sizeof(hello) - cluster_bytes);
	}

	if (type == ZEDBSD_FAT32) {
		unsigned lfn_count =
		    ((unsigned)strlen(long_name) + 12U) / 13U;

		for (i = 0; i < lfn_count; i++)
			set_lfn_entry(root + (i + 1U) * 32U, long_name,
			    lfn_count - i, lfn_checksum(long_sfn));
		set_dirent(root + (lfn_count + 1U) * 32U, long_sfn, 0x20U,
		    5U, sizeof(long_payload) - 1U, type);
		set_fat_entry(image, 5U, eoc);
		put_cluster_bytes(image, 5U, long_payload,
		    sizeof(long_payload) - 1U);
	}

	memset(&image->disk, 0, sizeof(image->disk));
	image->disk.d_dev = (dev_t)type;
	image->disk.d_block_size = SECTOR_SIZE;
	image->disk.d_block_count = image->sectors;
	image->disk.d_max_transfer_blocks = 32U;
	image->disk.d_data = image;
	check_fat_copies(image);
}

static void
destroy_image(struct memory_image *image)
{
	free(image->bytes);
	memset(image, 0, sizeof(*image));
}

static void
clone_image(const struct memory_image *source, struct memory_image *clone)
{
	*clone = *source;
	clone->bytes = malloc((size_t)source->sectors * SECTOR_SIZE);
	CHECK(clone->bytes != NULL);
	memcpy(clone->bytes, source->bytes,
	    (size_t)source->sectors * SECTOR_SIZE);
	clone->reads = 0;
	clone->direct_reads = 0;
	clone->writes = 0;
	clone->write_attempts = 0;
	clone->syncs = 0;
	clone->fail_reads = 0;
	clone->fail_writes = 0;
	clone->fail_write_attempt = 0;
	clone->fail_write_attempt2 = 0;
	clone->arm_read_after_writes = 0;
	clone->fail_syncs = 0;
	memset(&clone->disk, 0, sizeof(clone->disk));
	clone->disk.d_dev = (dev_t)clone->type;
	clone->disk.d_block_size = SECTOR_SIZE;
	clone->disk.d_block_count = clone->sectors;
	clone->disk.d_max_transfer_blocks = 32U;
	clone->disk.d_data = clone;
}

/* Small ordinary-VFS object helpers. */

static struct componentname
component(const char *name)
{
	struct componentname result;

	result.cn_nameptr = name;
	result.cn_namelen = strlen(name);
	result.cn_flags = COMPONENT_LAST;
	return result;
}

static int
lookup_child(struct inode *directory, const char *name, struct inode **result)
{
	struct componentname part = component(name);

	return directory->i_op->lookup(directory, &part, result);
}

static int
lookup_child_casefold(struct inode *directory, const char *name,
	struct inode **result)
{
	struct componentname part = component(name);

	return directory->i_op->lookup_casefold(directory, &part, result);
}

static int
lookup_path(struct mount *mountp, const char *path, struct inode **result)
{
	struct inode *current;
	const char *cursor = path;
	int error = 0;

	inode_ref(mountp->m_root);
	current = mountp->m_root;
	while (*cursor != '\0') {
		struct componentname part;
		struct inode *next;
		const char *slash = strchr(cursor, '/');

		part.cn_nameptr = cursor;
		part.cn_namelen = slash != NULL ? (size_t)(slash - cursor) :
		    strlen(cursor);
		part.cn_flags = slash == NULL ? COMPONENT_LAST : 0U;
		error = current->i_op->lookup(current, &part, &next);
		inode_release(current);
		if (error != 0)
			return error;
		current = next;
		if (slash == NULL)
			break;
		cursor = slash + 1;
	}
	*result = current;
	return 0;
}

static int
create_child(struct inode *directory, const char *name, struct inode **result)
{
	struct componentname part = component(name);
	const struct inode_creation_request request = {
		.origin = INODE_CREATION_SYSTEM,
		.type = INODE_REG,
		.mode = 0755U,
		.uid = 0,
		.gid = 0,
	};

	return directory->i_op->create(directory, &part, &request, result);
}

static int
mkdir_child(struct inode *directory, const char *name, struct inode **result)
{
	struct componentname part = component(name);
	const struct inode_creation_request request = {
		.origin = INODE_CREATION_SYSTEM,
		.type = INODE_DIR,
		.mode = 0755U,
		.uid = 0,
		.gid = 0,
	};

	return directory->i_op->mkdir(directory, &part, &request, result);
}

static int
unlink_child(struct inode *directory, const char *name)
{
	struct componentname part = component(name);

	return directory->i_op->unlink(directory, &part);
}

static int
rmdir_child(struct inode *directory, const char *name)
{
	struct componentname part = component(name);

	return directory->i_op->rmdir(directory, &part);
}

static int
rename_child(struct inode *old_directory, const char *old_name,
	struct inode *new_directory, const char *new_name)
{
	struct componentname old_part = component(old_name);
	struct componentname new_part = component(new_name);

	return old_directory->i_op->rename(old_directory, &old_part,
	    new_directory, &new_part, 0U);
}

static int
host_file_open(struct inode *inode, int flags, struct file *file)
{
	int error = 0;

	memset(file, 0, sizeof(*file));
	file->f_inode = inode;
	file->f_ops = inode->i_fop;
	file->f_path.p_mount = inode->i_mount;
	file->f_path.p_inode = inode;
	atomic_store_release(&file->f_flags, (unsigned)flags);
	refcount_init(&file->f_refs, 1U);
	inode_ref(inode);
	if (file->f_ops != NULL && file->f_ops->open != NULL)
		error = file->f_ops->open(file);
	if (error != 0) {
		inode_release(inode);
		memset(file, 0, sizeof(*file));
	}
	return error;
}

static int
host_file_close(struct file *file)
{
	struct inode *inode = file->f_inode;
	int error = 0;

	if (file->f_ops != NULL && file->f_ops->close != NULL)
		error = file->f_ops->close(file);
	inode_release(inode);
	memset(file, 0, sizeof(*file));
	return error;
}

static void
host_purge_mount_inodes(struct mount *mountp)
{
	unsigned i;

	if (mountp->m_root != NULL) {
		struct inode *root = mountp->m_root;

		inode_release(root);
		CHECK(refcount_load(&root->i_refs) == 1U);
		inode_free(root);
		mountp->m_root = NULL;
	}
	for (i = 0; i < HOST_INODE_MAX; i++) {
		struct inode *inode = host_inode_cache[i];

		if (inode == NULL || inode->i_mount != mountp)
			continue;
		if (refcount_load(&inode->i_refs) != 1U)
			fprintf(stderr,
			    "host inode leak: slot=%u ino=%llu type=%u flags=%#x "
			    "size=%lld refs=%u inode=%p mount=%p\n", i,
			    (unsigned long long)inode->i_ino, inode->i_type,
			    inode->i_flags, (long long)inode->i_size,
			    refcount_load(&inode->i_refs), (void *)inode,
			    (void *)inode->i_mount);
		CHECK(refcount_load(&inode->i_refs) == 1U);
		inode_free(inode);
	}
}

static int
host_mount(struct memory_image *image, unsigned flags, struct mount *mountp)
{
	memset(mountp, 0, sizeof(*mountp));
	mountp->m_disk = &image->disk;
	mountp->m_type = &fat_filesystem_type;
	mountp->m_flags = flags;
	refcount_init(&mountp->m_refs, 1U);
	return mountp->m_type->mount(mountp);
}

static void
host_unmount(struct mount *mountp)
{
	host_purge_mount_inodes(mountp);
	mountp->m_type->unmount(mountp);
	CHECK(mountp->m_data == NULL);
}

static void
host_check_idle_mount(struct mount *mountp)
{
	unsigned i;

	for (i = 0; i < HOST_INODE_MAX; i++) {
		struct inode *inode = host_inode_cache[i];
		unsigned wanted;

		if (inode == NULL || inode->i_mount != mountp)
			continue;
		wanted = inode == mountp->m_root ? 2U : 1U;
		if (refcount_load(&inode->i_refs) != wanted)
			fprintf(stderr,
			    "non-idle inode: slot=%u ino=%llu type=%u flags=%#x "
			    "size=%lld refs=%u wanted=%u\n", i,
			    (unsigned long long)inode->i_ino, inode->i_type,
			    inode->i_flags, (long long)inode->i_size,
			    refcount_load(&inode->i_refs), wanted);
		CHECK(refcount_load(&inode->i_refs) == wanted);
	}
}

static uint64_t
mount_free_blocks(struct mount *mountp)
{
	struct statvfs status;

	CHECK_ERROR(mountp->m_type->statvfs(mountp, &status), 0);
	CHECK(status.f_bfree == status.f_bavail);
	CHECK(status.f_bsize ==
	    (uint64_t)((struct memory_image *)mountp->m_disk->d_data)
	    ->sectors_per_cluster * SECTOR_SIZE);
	return status.f_bfree;
}

static int
capture_extent(uint64_t file_block, uint64_t disk_block, uint32_t count,
	void *argument)
{
	struct extent_capture *capture = argument;

	if (capture->reject)
		return EBUSY;
	if (capture->used >= ARRAY_COUNT(capture->count))
		return ENOSPC;
	capture->file_block[capture->used] = file_block;
	capture->disk_block[capture->used] = disk_block;
	capture->count[capture->used] = count;
	capture->used++;
	return 0;
}

static void
read_inode(struct inode *inode, void *buffer, size_t length)
{
	struct file file;

	CHECK_ERROR(host_file_open(inode, O_RDONLY, &file), 0);
	CHECK(file.f_ops->pread(&file, buffer, length, 0) == (ssize_t)length);
	CHECK_ERROR(host_file_close(&file), 0);
}

static void
read_path(struct mount *mountp, const char *path, const void *expected,
	size_t length)
{
	struct inode *inode;
	uint8_t buffer[1024];

	CHECK(length <= sizeof(buffer));
	CHECK_ERROR(lookup_path(mountp, path, &inode), 0);
	CHECK(inode->i_size == (off_t)length);
	read_inode(inode, buffer, length);
	CHECK(memcmp(buffer, expected, length) == 0);
	inode_release(inode);
}

static struct inode *
create_payload(struct inode *directory, const char *name, const void *payload,
	size_t length)
{
	struct inode *inode;
	struct file file;

	CHECK_ERROR(create_child(directory, name, &inode), 0);
	CHECK_ERROR(host_file_open(inode, O_RDWR, &file), 0);
	if (length != 0)
		CHECK(file.f_ops->write(&file, payload, length) ==
		    (ssize_t)length);
	CHECK_ERROR(file.f_ops->fsync(&file), 0);
	CHECK_ERROR(host_file_close(&file), 0);
	return inode;
}

static const char *
variant_name(enum bootfat_type type, const char *sfn, const char *lfn)
{
	return type == ZEDBSD_FAT32 ? lfn : sfn;
}

static void
check_probe_and_initial_contents(struct memory_image *image,
	struct mount *mountp, uint64_t *hello_identity)
{
	static const char long_name[] = "Long File Name.txt";
	struct inode *hello;
	struct file file;
	struct extent_capture capture = {0};
	struct disk *backing_disk;
	struct stat status;
	uint8_t expected[700];
	uint8_t actual[700];
	uint8_t boundary[10];
	uint64_t block;
	enum bootfat_type detected = 0;
	unsigned i;

	CHECK(strcmp(fat_filesystem_type.fs_name, "fat") == 0);
	CHECK_ERROR(fat_probe_type(&image->disk, &detected), 0);
	CHECK(detected == image->type);
	CHECK_ERROR(fat_filesystem_type.probe(&image->disk), 0);
	CHECK(mountp->m_root != NULL);
	CHECK(mountp->m_root->i_type == INODE_DIR);
	CHECK(mountp->m_root->i_ino == 1U);
	CHECK((mountp->m_root->i_mode & 07777U) == 0755U);
	CHECK(mountp->m_root->i_op != NULL);
	CHECK(mountp->m_root->i_fop != NULL);

	CHECK_ERROR(lookup_child(mountp->m_root, "HELLO.TXT", &hello), 0);
	CHECK(hello->i_type == INODE_REG);
	CHECK(hello->i_size == (off_t)sizeof(actual));
	CHECK((hello->i_mode & 07777U) == 0755U);
	CHECK_ERROR(hello->i_op->getattr(hello, &status), 0);
	CHECK(status.st_size == (off_t)sizeof(actual));
	CHECK(status.st_blocks == 2);
	CHECK_ERROR(fat_file_backing_identity(hello, &backing_disk,
	    hello_identity), 0);
	CHECK(backing_disk == &image->disk);
	CHECK_ERROR(fat_file_backing_identity(mountp->m_root, &backing_disk,
	    &block), EOPNOTSUPP);

	for (i = 0; i < ARRAY_COUNT(expected); i++)
		expected[i] = (uint8_t)('A' + i % 26U);
	CHECK_ERROR(host_file_open(hello, O_RDONLY, &file), 0);
	CHECK(file.f_ops->read(&file, actual, 509U) == 509);
	CHECK(file.f_ops->read(&file, actual + 509U,
	    sizeof(actual) - 509U) == (ssize_t)(sizeof(actual) - 509U));
	CHECK(file.f_offset == (off_t)sizeof(actual));
	CHECK(file.f_ops->read(&file, boundary, sizeof(boundary)) == 0);
	CHECK(file.f_ops->pread(&file, boundary, sizeof(boundary), 510) ==
	    (ssize_t)sizeof(boundary));
	CHECK(memcmp(boundary, expected + 510U, sizeof(boundary)) == 0);
	CHECK(memcmp(actual, expected, sizeof(actual)) == 0);

	CHECK_ERROR(fat_file_extents(&file, capture_extent, &capture), 0);
	CHECK(capture.used == 1U);
	CHECK(capture.file_block[0] == 0U);
	CHECK(capture.disk_block[0] == cluster_lba(image, 3U));
	CHECK(capture.count[0] == 2U);
	CHECK_ERROR(fat_file_contiguous_block(&file, &backing_disk, &block), 0);
	CHECK(backing_disk == &image->disk);
	CHECK(block == cluster_lba(image, 3U));
	capture.reject = 1;
	CHECK_ERROR(fat_file_extents(&file, capture_extent, &capture), EBUSY);
	CHECK_ERROR(host_file_close(&file), 0);
	inode_release(hello);

	{
		struct file directory;
		struct dirent entry;
		int eof = 0;
		int saw_hello = 0;
		int saw_long = 0;

		CHECK_ERROR(host_file_open(mountp->m_root, O_RDONLY | O_DIRECTORY,
		    &directory), 0);
		while (!eof) {
			CHECK_ERROR(directory.f_ops->readdir(&directory, &entry,
			    &eof), 0);
			if (eof)
				break;
			if (strcmp(entry.d_name,
			    image->type == ZEDBSD_FAT32 ? "HELLO.TXT" :
			    "hello.txt") == 0) {
				CHECK(entry.d_type == INODE_REG);
				saw_hello = 1;
			}
			if (strcmp(entry.d_name, long_name) == 0)
				saw_long = 1;
		}
		CHECK(saw_hello);
		CHECK(saw_long == (image->type == ZEDBSD_FAT32));
		CHECK_ERROR(host_file_close(&directory), 0);
	}

	if (image->type == ZEDBSD_FAT32) {
		struct inode *long_inode;
		char payload[12];

		CHECK_ERROR(lookup_child(mountp->m_root, long_name,
		    &long_inode), 0);
		read_inode(long_inode, payload, sizeof(payload));
		CHECK(memcmp(payload, "long payload", sizeof(payload)) == 0);
		inode_release(long_inode);
		CHECK_ERROR(lookup_child_casefold(mountp->m_root,
		    "long file name.TXT", &long_inode), 0);
		CHECK(long_inode->i_size == 12);
		inode_release(long_inode);
	} else {
		CHECK_ERROR(lookup_child_casefold(mountp->m_root, "hello.txt",
		    &hello), EOPNOTSUPP);
	}
}

static void
check_file_mutations(struct memory_image *image, struct mount *mountp)
{
	const char *name = variant_name(image->type, "RW.TXT",
	    "Created Long Name.txt");
	uint8_t sparse[10] = {
	    'a', 'Z', 'c', 'D', 'E', 'F', 0, 0, 'Q', '!',
	};
	uint8_t buffer[700];
	struct inode *inode;
	struct inode *again;
	struct file writer;
	struct file reader;
	struct file appender;
	struct file empty;
	struct extent_capture capture = {0};
	uint64_t free_before = mount_free_blocks(mountp);

	CHECK_ERROR(create_child(mountp->m_root, name, &inode), 0);
	CHECK(inode->i_size == 0);
	CHECK_ERROR(host_file_open(inode, O_RDWR, &writer), 0);
	CHECK(writer.f_ops->write(&writer, "abc", 3U) == 3);
	CHECK(writer.f_ops->write(&writer, "DEF", 3U) == 3);
	CHECK(writer.f_ops->pwrite(&writer, "Z", 1U, 1) == 1);
	CHECK(writer.f_ops->pwrite(&writer, "Q", 1U, 8) == 1);
	CHECK(inode->i_size == 9);
	CHECK(writer.f_ops->pread(&writer, buffer, 9U, 0) == 9);
	CHECK(memcmp(buffer, sparse, 9U) == 0);

	CHECK_ERROR(host_file_open(inode, O_RDONLY, &reader), 0);
	CHECK(reader.f_ops->pread(&reader, buffer, 9U, 0) == 9);
	CHECK(memcmp(buffer, sparse, 9U) == 0);
	CHECK_ERROR(host_file_open(inode, O_WRONLY | O_APPEND, &appender), 0);
	CHECK(appender.f_ops->write(&appender, "!", 1U) == 1);
	CHECK(writer.f_ops->pread(&writer, buffer, sizeof(sparse), 0) ==
	    (ssize_t)sizeof(sparse));
	CHECK(memcmp(buffer, sparse, sizeof(sparse)) == 0);
	CHECK_ERROR(host_file_close(&appender), 0);

	CHECK_ERROR(writer.f_ops->fsync(&writer), 0);
	CHECK_ERROR(inode->i_op->truncate(inode, 600), 0);
	CHECK(inode->i_size == 600);
	CHECK(reader.f_ops->pread(&reader, buffer, sizeof(buffer), 0) == 600);
	CHECK(memcmp(buffer, sparse, sizeof(sparse)) == 0);
	for (size_t i = sizeof(sparse); i < 600U; i++)
		CHECK(buffer[i] == 0);
	CHECK_ERROR(inode->i_op->truncate(inode, 2), 0);
	CHECK(inode->i_size == 2);
	CHECK(reader.f_ops->pread(&reader, buffer, sizeof(buffer), 0) == 2);
	CHECK(memcmp(buffer, "aZ", 2U) == 0);
	CHECK_ERROR(host_file_close(&reader), 0);
	CHECK_ERROR(host_file_close(&writer), 0);

	CHECK_ERROR(create_child(mountp->m_root, name, &again), EEXIST);
	CHECK(inode->i_size == 2);
	CHECK_ERROR(inode->i_op->truncate(inode, 0), 0);
	CHECK_ERROR(lookup_child(mountp->m_root, name, &again), 0);
	CHECK(again == inode);
	CHECK(again->i_size == 0);
	CHECK_ERROR(host_file_open(again, O_RDONLY, &empty), 0);
	CHECK_ERROR(fat_file_extents(&empty, capture_extent, &capture), 0);
	CHECK(capture.used == 0U);
	{
		struct disk *disk;
		uint64_t block;
		CHECK_ERROR(fat_file_contiguous_block(&empty, &disk, &block), EIO);
	}
	CHECK_ERROR(host_file_close(&empty), 0);
	inode_release(again);
	inode_release(inode);
	CHECK(mount_free_blocks(mountp) == free_before);
}

static void
check_open_writer_rename(struct memory_image *image, struct mount *mountp,
	struct inode *a, struct inode *b)
{
	const char *old_name = variant_name(image->type, "WRITER.TXT",
	    "Open Writer Long Name.txt");
	const char *new_name = variant_name(image->type, "WRITTEN.TXT",
	    "Renamed Writer Long Name.txt");
	struct memory_image rename_probe;
	struct mount probe_mount;
	struct inode *inode;
	struct inode *probe_a;
	struct inode *probe_b;
	struct file writer;
	unsigned writes_before;
	unsigned rename_writes;

	CHECK_ERROR(create_child(a, old_name, &inode), 0);
	CHECK_ERROR(host_file_open(inode, O_RDWR, &writer), 0);
	CHECK(writer.f_ops->write(&writer, "before", 6U) == 6);

	/* Measure this exact on-disk rename on a disposable snapshot, then arm a
	 * read fault after its final commit write.  The production rename must
	 * return the new dirent identity without a fallible post-commit read. */
	clone_image(image, &rename_probe);
	CHECK_ERROR(host_mount(&rename_probe, 0U, &probe_mount), 0);
	CHECK_ERROR(lookup_child(probe_mount.m_root, "A", &probe_a), 0);
	CHECK_ERROR(lookup_child(probe_mount.m_root, "B", &probe_b), 0);
	writes_before = rename_probe.writes;
	CHECK_ERROR(rename_child(probe_a, old_name, probe_b, new_name), 0);
	rename_writes = rename_probe.writes - writes_before;
	CHECK(rename_writes != 0U);
	inode_release(probe_a);
	inode_release(probe_b);
	host_unmount(&probe_mount);
	destroy_image(&rename_probe);

	image->arm_read_after_writes = image->writes + rename_writes;
	CHECK_ERROR(rename_child(a, old_name, b, new_name), 0);
	CHECK(image->arm_read_after_writes == 0U);
	CHECK(image->fail_reads == 1U);
	image->fail_reads = 0U;
	CHECK_ERROR(lookup_child(a, old_name, &inode), ENOENT);
	CHECK(writer.f_ops->write(&writer, "after", 5U) == 5);
	CHECK_ERROR(writer.f_ops->fsync(&writer), 0);
	CHECK_ERROR(host_file_close(&writer), 0);
	inode_release(inode);
	{
		char path[96];
		snprintf(path, sizeof(path), "B/%s", new_name);
		read_path(mountp, path, "beforeafter", 11U);
	}
}

static void
check_namespace_and_orphans(struct memory_image *image, struct mount *mountp)
{
	const char *child_name = variant_name(image->type, "CHILD.TXT",
	    "Child Long Name.txt");
	const char *renamed_name = variant_name(image->type, "RENAMED.TXT",
	    "Renamed Long Name.txt");
	const char *moved_name = variant_name(image->type, "MOVED.TXT",
	    "Moved Long Name.txt");
	const char *target_name = variant_name(image->type, "TARGET.TXT",
	    "Target Long Name.txt");
	struct inode *a;
	struct inode *b;
	struct inode *file;
	struct inode *target;
	struct file target_handle;
	uint64_t free_before_target_close;
	char buffer[16];

	CHECK_ERROR(mkdir_child(mountp->m_root, "A", &a), 0);
	CHECK_ERROR(mkdir_child(mountp->m_root, "B", &b), 0);
	file = create_payload(a, child_name, "source", 6U);
	inode_release(file);
	CHECK_ERROR(rmdir_child(mountp->m_root, "A"), ENOTEMPTY);
	CHECK_ERROR(rename_child(a, child_name, a, renamed_name), 0);
	CHECK_ERROR(lookup_child(a, child_name, &file), ENOENT);
	CHECK_ERROR(rename_child(a, renamed_name, b, moved_name), 0);
	{
		char path[96];
		current_stage = "cross-directory rename readback";
		snprintf(path, sizeof(path), "B/%s", moved_name);
		read_path(mountp, path, "source", 6U);
	}

	target = create_payload(b, target_name, "target", 6U);
	CHECK_ERROR(host_file_open(target, O_RDONLY, &target_handle), 0);
	inode_release(target);
	CHECK_ERROR(rename_child(b, moved_name, b, target_name), 0);
	CHECK_ERROR(lookup_child(b, moved_name, &file), ENOENT);
	{
		char path[96];
		current_stage = "replacement rename readback";
		snprintf(path, sizeof(path), "B/%s", target_name);
		read_path(mountp, path, "source", 6U);
	}
	CHECK(target_handle.f_ops->pread(&target_handle, buffer, 6U, 0) == 6);
	CHECK(memcmp(buffer, "target", 6U) == 0);
	free_before_target_close = mount_free_blocks(mountp);
	CHECK_ERROR(host_file_close(&target_handle), 0);
	CHECK(mount_free_blocks(mountp) == free_before_target_close + 1U);

	check_open_writer_rename(image, mountp, a, b);

	{
		const char *orphan_name = variant_name(image->type, "ORPHAN.TXT",
		    "Orphan Long Name.txt");
		struct inode *orphan = create_payload(b, orphan_name,
		    "orphan", 6U);
		struct file orphan_handle;
		uint64_t free_before_unlink;

		CHECK_ERROR(host_file_open(orphan, O_RDONLY, &orphan_handle), 0);
		inode_release(orphan);
		free_before_unlink = mount_free_blocks(mountp);
		CHECK_ERROR(unlink_child(b, orphan_name), 0);
		CHECK(mount_free_blocks(mountp) == free_before_unlink);
		CHECK_ERROR(lookup_child(b, orphan_name, &orphan), ENOENT);
		CHECK(orphan_handle.f_ops->pread(&orphan_handle, buffer, 6U, 0) ==
		    6);
		CHECK(memcmp(buffer, "orphan", 6U) == 0);
		CHECK_ERROR(host_file_close(&orphan_handle), 0);
		CHECK(mount_free_blocks(mountp) == free_before_unlink + 1U);
	}

	{
		struct inode *sub;
		struct inode *descendant;
		struct inode *moved_sub;
		struct inode *fresh_descendant;

		CHECK_ERROR(mkdir_child(a, "SUB", &sub), 0);
		descendant = create_payload(sub, "DEEP.TXT", "deep", 4U);
		CHECK_ERROR(rename_child(a, "SUB", b, "SUB"), 0);
		CHECK_ERROR(lookup_child(b, "SUB", &moved_sub), 0);
		CHECK(moved_sub == sub);
		CHECK_ERROR(lookup_child(moved_sub, "DEEP.TXT",
		    &fresh_descendant), 0);
		CHECK(fresh_descendant == descendant);
		inode_release(fresh_descendant);
		current_stage = "renamed directory descendant readback";
		read_inode(descendant, buffer, 4U);
		CHECK(memcmp(buffer, "deep", 4U) == 0);
		CHECK_ERROR(rmdir_child(b, "SUB"), ENOTEMPTY);
		CHECK_ERROR(unlink_child(moved_sub, "DEEP.TXT"), 0);
		inode_release(descendant);
		inode_release(moved_sub);
		inode_release(sub);
		CHECK_ERROR(rmdir_child(b, "SUB"), 0);
	}

	CHECK_ERROR(unlink_child(b, target_name), 0);
	CHECK_ERROR(unlink_child(b, variant_name(image->type, "WRITTEN.TXT",
	    "Renamed Writer Long Name.txt")), 0);
	CHECK_ERROR(unlink_child(mountp->m_root, "A"), EISDIR);
	CHECK_ERROR(rmdir_child(mountp->m_root, "A"), 0);
	CHECK_ERROR(rmdir_child(mountp->m_root, "B"), 0);
	inode_release(a);
	inode_release(b);
	CHECK_ERROR(mkdir_child(mountp->m_root, "EMPTY", &a), 0);
	inode_release(a);
	CHECK_ERROR(rmdir_child(mountp->m_root, "EMPTY"), 0);
}

static void
check_rename_descendant_path_preflight(struct memory_image *image,
	struct mount *mountp)
{
	char child_name[101];
	char new_name[201];
	char buffer[4];
	struct inode *source;
	struct inode *source_again;
	struct inode *descendant;
	struct inode *descendant_again;
	unsigned writes_before;

	if (image->type != ZEDBSD_FAT32)
		return;
	current_stage = "rename descendant path-length preflight";
	memset(child_name, 'c', sizeof(child_name) - 1U);
	child_name[sizeof(child_name) - 1U] = '\0';
	memset(new_name, 'N', sizeof(new_name) - 1U);
	new_name[sizeof(new_name) - 1U] = '\0';
	CHECK_ERROR(mkdir_child(mountp->m_root, "S", &source), 0);
	descendant = create_payload(source, child_name, "path", 4U);
	writes_before = image->writes;
	CHECK_ERROR(rename_child(mountp->m_root, "S", mountp->m_root,
	    new_name), ENAMETOOLONG);
	CHECK(image->writes == writes_before);
	CHECK_ERROR(lookup_child(mountp->m_root, "S", &source_again), 0);
	CHECK(source_again == source);
	CHECK_ERROR(lookup_child(source_again, child_name, &descendant_again), 0);
	CHECK(descendant_again == descendant);
	read_inode(descendant, buffer, sizeof(buffer));
	CHECK(memcmp(buffer, "path", sizeof(buffer)) == 0);
	inode_release(descendant_again);
	descendant_again = NULL;
	CHECK_ERROR(lookup_child(mountp->m_root, new_name, &descendant_again),
	    ENOENT);
	inode_release(descendant);
	CHECK_ERROR(unlink_child(source, child_name), 0);
	inode_release(source_again);
	inode_release(source);
	CHECK_ERROR(rmdir_child(mountp->m_root, "S"), 0);
}

static void
check_io_fault_retry(struct mount *mountp)
{
	struct memory_image *image = mountp->m_disk->d_data;
	struct inode *hello;
	struct file file;
	char byte;

	CHECK_ERROR(lookup_child(mountp->m_root, "HELLO.TXT", &hello), 0);
	CHECK_ERROR(host_file_open(hello, O_RDWR, &file), 0);
	image->fail_writes = 1U;
	CHECK(file.f_ops->pwrite(&file, "z", 1U, 0) == -EIO);
	CHECK(file.f_ops->pwrite(&file, "z", 1U, 0) == 1);
	image->fail_syncs = 1U;
	CHECK_ERROR(file.f_ops->fsync(&file), EIO);
	CHECK_ERROR(file.f_ops->fsync(&file), 0);
	CHECK(file.f_ops->pread(&file, &byte, 1U, 0) == 1);
	CHECK(byte == 'z');
	CHECK_ERROR(host_file_close(&file), 0);
	inode_release(hello);
}

static void
check_mount_sync_open_writer(struct memory_image *image, struct mount *mountp)
{
	static const char payload[] = "mount-sync durable";
	const char *name = variant_name(image->type, "MTSYNC.TXT",
	    "Mount Sync Long Name.txt");
	struct memory_image snapshot;
	struct mount snapshot_mount;
	struct inode *inode;
	struct inode *snapshot_inode;
	struct file writer;
	struct file reader;
	uint8_t zeros[513];
	char actual[sizeof(payload) - 1U];
	off_t wanted_size = (off_t)(sizeof(zeros) + sizeof(actual));
	unsigned writes_after_sync;
	unsigned syncs_before = image->syncs;

	current_stage = "mount sync with live dirty writer";
	CHECK_ERROR(create_child(mountp->m_root, name, &inode), 0);
	CHECK_ERROR(host_file_open(inode, O_RDWR, &writer), 0);
	CHECK(writer.f_ops->pwrite(&writer, payload, sizeof(payload) - 1U,
	    (off_t)sizeof(zeros)) == (ssize_t)(sizeof(payload) - 1U));
	CHECK(inode->i_size == wanted_size);
	CHECK_ERROR(mountp->m_type->sync(mountp), 0);
	CHECK(image->syncs == syncs_before + 1U);
	writes_after_sync = image->writes;

	/* A second mount of a byte-for-byte durable snapshot must not depend on
	 * the original open description or its in-memory inode authority. */
	clone_image(image, &snapshot);
	CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY, &snapshot_mount), 0);
	CHECK_ERROR(lookup_child(snapshot_mount.m_root, name, &snapshot_inode), 0);
	CHECK(snapshot_inode->i_size == wanted_size);
	CHECK_ERROR(host_file_open(snapshot_inode, O_RDONLY, &reader), 0);
	CHECK(reader.f_ops->pread(&reader, zeros, sizeof(zeros), 0) ==
	    (ssize_t)sizeof(zeros));
	for (size_t i = 0; i < sizeof(zeros); i++)
		CHECK(zeros[i] == 0);
	CHECK(reader.f_ops->pread(&reader, actual, sizeof(actual),
	    (off_t)sizeof(zeros)) == (ssize_t)sizeof(actual));
	CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
	CHECK_ERROR(host_file_close(&reader), 0);
	inode_release(snapshot_inode);
	host_unmount(&snapshot_mount);
	destroy_image(&snapshot);

	CHECK_ERROR(host_file_close(&writer), 0);
	CHECK(image->writes == writes_after_sync);
	inode_release(inode);
}

static void
check_open_writer_path_truncate(struct memory_image *image,
	struct mount *mountp)
{
	const char *name = variant_name(image->type, "AUTH.TXT",
	    "Writer Authority Long Name.txt");
	struct memory_image snapshot;
	struct mount snapshot_mount;
	struct inode *inode;
	struct file writer;
	struct file reader;
	struct extent_capture capture = {0};
	uint8_t payload[513];
	uint8_t actual[513];
	uint64_t free_before = mount_free_blocks(mountp);
	uint32_t covered = 0;
	unsigned i;

	current_stage = "open-writer path truncate authority";
	for (i = 0; i < ARRAY_COUNT(payload); i++)
		payload[i] = (uint8_t)('0' + i % 10U);
	CHECK_ERROR(create_child(mountp->m_root, name, &inode), 0);
	CHECK_ERROR(host_file_open(inode, O_RDWR, &writer), 0);
	CHECK(writer.f_ops->pwrite(&writer, payload, sizeof(payload), 0) ==
	    (ssize_t)sizeof(payload));
	CHECK(inode->i_size == (off_t)sizeof(payload));
	/* The second open must inherit inode authority even though the directory
	 * entry still describes the empty file created on disk. */
	CHECK_ERROR(host_file_open(inode, O_RDONLY, &reader), 0);
	CHECK(reader.f_ops->pread(&reader, actual, sizeof(actual), 0) ==
	    (ssize_t)sizeof(actual));
	CHECK(memcmp(actual, payload, sizeof(actual)) == 0);

	CHECK_ERROR(inode->i_op->truncate(inode, 100), 0);
	CHECK(inode->i_size == 100);
	CHECK(writer.f_ops->pread(&writer, actual, sizeof(actual), 0) == 100);
	CHECK(memcmp(actual, payload, 100U) == 0);
	CHECK(reader.f_ops->pread(&reader, actual, 1U, 100) == 0);
	CHECK(mount_free_blocks(mountp) == free_before - 1U);
	CHECK_ERROR(fat_file_extents(&writer, capture_extent, &capture), 0);
	for (i = 0; i < capture.used; i++) {
		CHECK(capture.file_block[i] == covered);
		covered += capture.count[i];
	}
	CHECK(covered == 1U);

	CHECK_ERROR(mountp->m_type->sync(mountp), 0);
	clone_image(image, &snapshot);
	CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY, &snapshot_mount), 0);
	read_path(&snapshot_mount, name, payload, 100U);
	CHECK(mount_free_blocks(&snapshot_mount) == free_before - 1U);
	host_unmount(&snapshot_mount);
	destroy_image(&snapshot);

	CHECK_ERROR(host_file_close(&reader), 0);
	CHECK_ERROR(host_file_close(&writer), 0);
	inode_release(inode);
	CHECK_ERROR(unlink_child(mountp->m_root, name), 0);
	CHECK(mount_free_blocks(mountp) == free_before);
}

static void
check_orphan_fd_mutation(struct memory_image *image, struct mount *mountp)
{
	const char *name = variant_name(image->type, "ORPHFD.TXT",
	    "Orphan Descriptor Mutation.txt");
	static const uint8_t marker[] = {'l', 'i', 'v', 'e'};
	struct inode *inode;
	struct inode *absent = NULL;
	struct file file;
	struct extent_capture capture = {0};
	uint8_t initial[700];
	uint8_t actual[700];
	uint64_t free_before = mount_free_blocks(mountp);
	uint32_t covered = 0;
	unsigned i;

	current_stage = "unlinked descriptor truncate and grow";
	for (i = 0; i < ARRAY_COUNT(initial); i++)
		initial[i] = (uint8_t)('A' + i % 23U);
	inode = create_payload(mountp->m_root, name, initial, sizeof(initial));
	CHECK_ERROR(host_file_open(inode, O_RDWR, &file), 0);
	inode_release(inode);
	CHECK(mount_free_blocks(mountp) == free_before - 2U);
	CHECK_ERROR(unlink_child(mountp->m_root, name), 0);
	CHECK_ERROR(lookup_child(mountp->m_root, name, &absent), ENOENT);
	CHECK(mount_free_blocks(mountp) == free_before - 2U);

	CHECK_ERROR(inode->i_op->truncate(inode, 100), 0);
	CHECK(inode->i_size == 100);
	CHECK(file.f_ops->pread(&file, actual, sizeof(actual), 0) == 100);
	CHECK(memcmp(actual, initial, 100U) == 0);
	CHECK(mount_free_blocks(mountp) == free_before - 1U);
	CHECK(file.f_ops->pwrite(&file, marker, sizeof(marker), 100) ==
	    (ssize_t)sizeof(marker));
	CHECK(inode->i_size == 104);
	CHECK_ERROR(inode->i_op->truncate(inode, 600), 0);
	CHECK(inode->i_size == 600);
	CHECK(file.f_ops->pread(&file, actual, sizeof(actual), 0) == 600);
	CHECK(memcmp(actual, initial, 100U) == 0);
	CHECK(memcmp(actual + 100U, marker, sizeof(marker)) == 0);
	for (i = 100U + sizeof(marker); i < 600U; i++)
		CHECK(actual[i] == 0);
	CHECK(mount_free_blocks(mountp) == free_before - 2U);
	CHECK_ERROR(fat_file_extents(&file, capture_extent, &capture), 0);
	for (i = 0; i < capture.used; i++) {
		CHECK(capture.file_block[i] == covered);
		covered += capture.count[i];
	}
	CHECK(covered == 2U);
	CHECK_ERROR(file.f_ops->fsync(&file), 0);
	CHECK_ERROR(lookup_child(mountp->m_root, name, &absent), ENOENT);
	CHECK_ERROR(host_file_close(&file), 0);
	CHECK(mount_free_blocks(mountp) == free_before);
	CHECK_ERROR(lookup_child(mountp->m_root, name, &absent), ENOENT);
	check_fat_copies(image);
}

static void
check_orphan_reclaim_retry_one(struct memory_image *image,
	struct mount *mountp, int allocation_failure)
{
	const char *orphan_name = allocation_failure ?
	    variant_name(image->type, "ORALC.TXT",
		"Orphan Allocation Retry.txt") :
	    variant_name(image->type, "ORWRT.TXT", "Orphan Write Retry.txt");
	const char *gate_name = allocation_failure ? "GATEA.TXT" : "GATEW.TXT";
	struct memory_image snapshot;
	struct mount snapshot_mount;
	struct inode *inode;
	struct inode *gate;
	struct inode *absent = NULL;
	struct file file;
	uint8_t payload[700];
	uint64_t free_before = mount_free_blocks(mountp);
	unsigned i;
	int wanted_error = allocation_failure ? ENOMEM : EIO;

	current_stage = allocation_failure ?
	    "deferred orphan allocation failure" :
	    "deferred orphan write failure";
	for (i = 0; i < ARRAY_COUNT(payload); i++)
		payload[i] = (uint8_t)('a' + i % 19U);
	inode = create_payload(mountp->m_root, orphan_name, payload,
	    sizeof(payload));
	gate = create_payload(mountp->m_root, gate_name, "gate", 4U);
	inode_release(gate);
	CHECK_ERROR(host_file_open(inode, O_RDWR, &file), 0);
	inode_release(inode);
	CHECK_ERROR(unlink_child(mountp->m_root, orphan_name), 0);
	CHECK_ERROR(lookup_child(mountp->m_root, orphan_name, &absent), ENOENT);
	CHECK_ERROR(file.f_ops->fsync(&file), 0);
	CHECK(mount_free_blocks(mountp) == free_before - 3U);

	if (allocation_failure)
		fail_kern_allocations = 1U;
	else
		image->fail_writes = 1U;
	/* Reclaim runs from the final inode_release(), after close detached the
	 * open description.  Its error is retained by the mount, not returned
	 * through close. */
	CHECK_ERROR(host_file_close(&file), 0);
	if (allocation_failure)
		CHECK(fail_kern_allocations == 0U);
	else
		CHECK(image->fail_writes == 0U);
	CHECK(mount_free_blocks(mountp) == free_before - 3U);
	CHECK_ERROR(lookup_child(mountp->m_root, orphan_name, &absent), ENOENT);
	check_fat_copies(image);

	/* Namespace removal must not overtake a pending reclaim which cannot be
	 * drained.  The target remains visible and unchanged after this fault. */
	if (allocation_failure)
		fail_kern_allocations = 1U;
	else
		image->fail_writes = 1U;
	CHECK_ERROR(unlink_child(mountp->m_root, gate_name), wanted_error);
	if (allocation_failure)
		CHECK(fail_kern_allocations == 0U);
	else
		CHECK(image->fail_writes == 0U);
	CHECK_ERROR(lookup_child(mountp->m_root, gate_name, &gate), 0);
	CHECK(gate->i_size == 4);
	inode_release(gate);
	CHECK(mount_free_blocks(mountp) == free_before - 3U);

	CHECK_ERROR(mountp->m_type->sync(mountp), 0);
	CHECK(mount_free_blocks(mountp) == free_before - 1U);
	CHECK_ERROR(lookup_child(mountp->m_root, orphan_name, &absent), ENOENT);
	check_fat_copies(image);
	clone_image(image, &snapshot);
	CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY, &snapshot_mount), 0);
	CHECK_ERROR(lookup_child(snapshot_mount.m_root, orphan_name, &absent),
	    ENOENT);
	read_path(&snapshot_mount, gate_name, "gate", 4U);
	CHECK(mount_free_blocks(&snapshot_mount) == free_before - 1U);
	host_unmount(&snapshot_mount);
	destroy_image(&snapshot);

	CHECK_ERROR(unlink_child(mountp->m_root, gate_name), 0);
	CHECK(mount_free_blocks(mountp) == free_before);
	check_fat_copies(image);
}

static void
check_orphan_reclaim_retry(struct memory_image *image, struct mount *mountp)
{
	check_orphan_reclaim_retry_one(image, mountp, 0);
	check_orphan_reclaim_retry_one(image, mountp, 1);
}

static void
check_close_metadata_retry(struct memory_image *image, struct mount *mountp)
{
	const char *name = variant_name(image->type, "CLFAIL.TXT",
	    "Close Metadata Retry.txt");
	struct memory_image snapshot;
	struct mount snapshot_mount;
	struct inode *inode;
	struct inode *snapshot_inode;
	struct file writer;
	struct file snapshot_file;
	struct extent_capture capture = {0};
	uint8_t payload[700];
	uint8_t actual[700];
	uint64_t free_before = mount_free_blocks(mountp);
	uint32_t covered = 0;
	unsigned i;

	current_stage = "close metadata failure and mount-sync retry";
	for (i = 0; i < ARRAY_COUNT(payload); i++)
		payload[i] = (uint8_t)('0' + i % 10U);
	CHECK_ERROR(create_child(mountp->m_root, name, &inode), 0);
	CHECK_ERROR(host_file_open(inode, O_RDWR, &writer), 0);
	CHECK(writer.f_ops->pwrite(&writer, payload, sizeof(payload), 0) ==
	    (ssize_t)sizeof(payload));
	CHECK(inode->i_size == (off_t)sizeof(payload));
	CHECK(mount_free_blocks(mountp) == free_before - 2U);

	/* Data and FAT allocation have completed.  The next write is the dirty
	 * SFN directory entry which close must retain as a mount-owned retry. */
	image->fail_writes = 1U;
	CHECK_ERROR(host_file_close(&writer), EIO);
	CHECK(image->fail_writes == 0U);
	CHECK(refcount_load(&inode->i_refs) == 2U);
	CHECK(inode->i_size == (off_t)sizeof(payload));
	CHECK(mount_free_blocks(mountp) == free_before - 2U);

	CHECK_ERROR(mountp->m_type->sync(mountp), 0);
	CHECK(refcount_load(&inode->i_refs) == 2U);
	CHECK(mount_free_blocks(mountp) == free_before - 2U);
	clone_image(image, &snapshot);
	CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY, &snapshot_mount), 0);
	CHECK_ERROR(lookup_child(snapshot_mount.m_root, name, &snapshot_inode), 0);
	CHECK(snapshot_inode->i_size == (off_t)sizeof(payload));
	CHECK_ERROR(host_file_open(snapshot_inode, O_RDONLY, &snapshot_file), 0);
	CHECK(snapshot_file.f_ops->pread(&snapshot_file, actual,
	    sizeof(actual), 0) == (ssize_t)sizeof(actual));
	CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
	CHECK_ERROR(fat_file_extents(&snapshot_file, capture_extent, &capture), 0);
	for (i = 0; i < capture.used; i++) {
		CHECK(capture.file_block[i] == covered);
		covered += capture.count[i];
	}
	CHECK(covered == 2U);
	CHECK_ERROR(host_file_close(&snapshot_file), 0);
	inode_release(snapshot_inode);
	CHECK(mount_free_blocks(&snapshot_mount) == free_before - 2U);
	host_unmount(&snapshot_mount);
	destroy_image(&snapshot);

	inode_release(inode);
	CHECK_ERROR(unlink_child(mountp->m_root, name), 0);
	CHECK(mount_free_blocks(mountp) == free_before);
	check_fat_copies(image);
}

static void
check_orphan_setattr_slot_reuse(void)
{
	static const enum bootfat_type types[] = {
		ZEDBSD_FAT12, ZEDBSD_FAT16, ZEDBSD_FAT32,
	};
	static const char old_name[] = "STALEA.TXT";
	static const char new_name[] = "STALEB.TXT";
	static const char old_payload[] = "old descriptor";
	static const char new_payload[] = "new slot owner";
	unsigned type_index;

	for (type_index = 0; type_index < ARRAY_COUNT(types); type_index++) {
		struct memory_image image;
		struct mount mountp;
		struct inode *old_inode;
		struct inode *new_inode;
		struct file old_file;
		struct disk *old_disk;
		struct disk *new_disk;
		struct stat baseline;
		struct stat after;
		struct stat requested;
		uint64_t old_identity;
		uint64_t new_identity;
		uint64_t free_before;
		unsigned writes_before;
		char actual[sizeof(old_payload) - 1U];

		current_type = types[type_index];
		current_stage = "dead inode setattr after direntry reuse";
		current_fault_ordinal = 0;
		format_image(&image, current_type, 1U, 2U);
		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		free_before = mount_free_blocks(&mountp);
		old_inode = create_payload(mountp.m_root, old_name, old_payload,
		    sizeof(old_payload) - 1U);
		CHECK_ERROR(host_file_open(old_inode, O_RDWR, &old_file), 0);
		CHECK_ERROR(fat_file_backing_identity(old_inode, &old_disk,
		    &old_identity), 0);
		CHECK(old_disk == &image.disk);
		CHECK_ERROR(unlink_child(mountp.m_root, old_name), 0);
		CHECK((old_inode->i_flags & INODE_DEAD) != 0);

		new_inode = create_payload(mountp.m_root, new_name, new_payload,
		    sizeof(new_payload) - 1U);
		CHECK_ERROR(fat_file_backing_identity(new_inode, &new_disk,
		    &new_identity), 0);
		CHECK(new_disk == &image.disk);
		CHECK(new_identity == old_identity);
		CHECK_ERROR(new_inode->i_op->getattr(new_inode, &baseline), 0);
		memset(&requested, 0, sizeof(requested));
		requested.st_mode = S_IFREG | 0555U;
		requested.st_atim.tv_sec = 1700000000;
		requested.st_mtim.tv_sec = 1700000122;
		writes_before = image.writes;
		CHECK_ERROR(old_inode->i_op->setattr(old_inode, &requested,
		    INODE_ATTR_MODE | INODE_ATTR_ATIME | INODE_ATTR_MTIME), 0);
		CHECK(image.writes == writes_before);
		CHECK(old_file.f_ops->pread(&old_file, actual, sizeof(actual), 0) ==
		    (ssize_t)sizeof(actual));
		CHECK(memcmp(actual, old_payload, sizeof(actual)) == 0);
		read_inode(new_inode, actual, sizeof(new_payload) - 1U);
		CHECK(memcmp(actual, new_payload, sizeof(new_payload) - 1U) == 0);

		inode_release(old_inode);
		CHECK_ERROR(host_file_close(&old_file), 0);
		CHECK(mount_free_blocks(&mountp) == free_before - 1U);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
		inode_release(new_inode);
		host_unmount(&mountp);
		check_fat_copies(&image);

		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		CHECK_ERROR(lookup_child(mountp.m_root, new_name, &new_inode), 0);
		CHECK_ERROR(new_inode->i_op->getattr(new_inode, &after), 0);
		CHECK(after.st_mode == baseline.st_mode);
		CHECK(after.st_size == baseline.st_size);
		CHECK(after.st_atime == baseline.st_atime);
		CHECK(after.st_mtime == baseline.st_mtime);
		CHECK(after.st_ctime == baseline.st_ctime);
		CHECK_ERROR(fat_file_backing_identity(new_inode, &new_disk,
		    &new_identity), 0);
		CHECK(new_disk == &image.disk);
		CHECK(new_identity == old_identity);
		read_inode(new_inode, actual, sizeof(new_payload) - 1U);
		CHECK(memcmp(actual, new_payload, sizeof(new_payload) - 1U) == 0);
		inode_release(new_inode);
		CHECK_ERROR(unlink_child(mountp.m_root, new_name), 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		host_unmount(&mountp);
		destroy_image(&image);
	}
}

static void
check_replacement_payload_write_failure(void)
{
	static const enum bootfat_type types[] = {
		ZEDBSD_FAT12, ZEDBSD_FAT16, ZEDBSD_FAT32,
	};
	static const char source_payload[] = "source replacement payload";
	static const char target_payload[] = "target preserved";
	unsigned type_index;

	for (type_index = 0; type_index < ARRAY_COUNT(types); type_index++) {
		struct memory_image image;
		struct memory_image snapshot;
		struct mount mountp;
		struct mount snapshot_mount;
		struct inode *source;
		struct inode *target;
		struct inode *again;
		struct inode *absent = NULL;
		struct file target_handle;
		struct disk *disk;
		uint64_t source_identity;
		uint64_t target_identity;
		uint64_t found_identity;
		uint64_t free_initial;
		uint64_t free_before;
		char actual[sizeof(source_payload) - 1U];
		const char *source_name;
		const char *target_name;

		current_type = types[type_index];
		current_stage = "replacement destination payload write failure";
		current_fault_ordinal = 1U;
		source_name = variant_name(current_type, "RNSRC.TXT",
		    "Replacement Source Long Name.txt");
		target_name = variant_name(current_type, "RNDST.TXT",
		    "Replacement Target Long Name.txt");
		format_image(&image, current_type, 1U, 2U);
		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		free_initial = mount_free_blocks(&mountp);
		source = create_payload(mountp.m_root, source_name,
		    source_payload, sizeof(source_payload) - 1U);
		target = create_payload(mountp.m_root, target_name,
		    target_payload, sizeof(target_payload) - 1U);
		CHECK_ERROR(fat_file_backing_identity(source, &disk,
		    &source_identity), 0);
		CHECK(disk == &image.disk);
		CHECK_ERROR(fat_file_backing_identity(target, &disk,
		    &target_identity), 0);
		CHECK(disk == &image.disk);
		CHECK(source_identity != target_identity);
		CHECK_ERROR(host_file_open(target, O_RDONLY, &target_handle), 0);
		free_before = mount_free_blocks(&mountp);
		CHECK(free_before == free_initial - 2U);

		image.fail_writes = 1U;
		CHECK_ERROR(rename_child(mountp.m_root, source_name,
		    mountp.m_root, target_name), EIO);
		CHECK(image.fail_writes == 0U);
		CHECK((source->i_flags & INODE_DEAD) == 0);
		CHECK((target->i_flags & INODE_DEAD) == 0);
		CHECK_ERROR(lookup_child(mountp.m_root, source_name, &again), 0);
		CHECK(again == source);
		CHECK_ERROR(fat_file_backing_identity(again, &disk,
		    &found_identity), 0);
		CHECK(disk == &image.disk);
		CHECK(found_identity == source_identity);
		read_inode(again, actual, sizeof(source_payload) - 1U);
		CHECK(memcmp(actual, source_payload,
		    sizeof(source_payload) - 1U) == 0);
		inode_release(again);
		CHECK_ERROR(lookup_child(mountp.m_root, target_name, &again), 0);
		CHECK(again == target);
		CHECK_ERROR(fat_file_backing_identity(again, &disk,
		    &found_identity), 0);
		CHECK(disk == &image.disk);
		CHECK(found_identity == target_identity);
		read_inode(again, actual, sizeof(target_payload) - 1U);
		CHECK(memcmp(actual, target_payload,
		    sizeof(target_payload) - 1U) == 0);
		inode_release(again);
		CHECK(target_handle.f_ops->pread(&target_handle, actual,
		    sizeof(target_payload) - 1U, 0) ==
		    (ssize_t)(sizeof(target_payload) - 1U));
		CHECK(memcmp(actual, target_payload,
		    sizeof(target_payload) - 1U) == 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);

		clone_image(&image, &snapshot);
		CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY,
		    &snapshot_mount), 0);
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, source_name,
		    &again), 0);
		CHECK_ERROR(fat_file_backing_identity(again, &disk,
		    &found_identity), 0);
		CHECK(disk == &snapshot.disk);
		CHECK(found_identity == source_identity);
		inode_release(again);
		read_path(&snapshot_mount, source_name, source_payload,
		    sizeof(source_payload) - 1U);
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, target_name,
		    &again), 0);
		CHECK_ERROR(fat_file_backing_identity(again, &disk,
		    &found_identity), 0);
		CHECK(disk == &snapshot.disk);
		CHECK(found_identity == target_identity);
		inode_release(again);
		read_path(&snapshot_mount, target_name, target_payload,
		    sizeof(target_payload) - 1U);
		CHECK(mount_free_blocks(&snapshot_mount) == free_before);
		host_unmount(&snapshot_mount);
		destroy_image(&snapshot);

		CHECK_ERROR(rename_child(mountp.m_root, source_name,
		    mountp.m_root, target_name), 0);
		CHECK((target->i_flags & INODE_DEAD) != 0);
		absent = NULL;
		CHECK_ERROR(lookup_child(mountp.m_root, source_name, &absent),
		    ENOENT);
		CHECK_ERROR(lookup_child(mountp.m_root, target_name, &again), 0);
		CHECK(again == source);
		CHECK_ERROR(fat_file_backing_identity(again, &disk,
		    &found_identity), 0);
		CHECK(disk == &image.disk);
		CHECK(found_identity == target_identity);
		read_inode(again, actual, sizeof(source_payload) - 1U);
		CHECK(memcmp(actual, source_payload,
		    sizeof(source_payload) - 1U) == 0);
		inode_release(again);
		CHECK(target_handle.f_ops->pread(&target_handle, actual,
		    sizeof(target_payload) - 1U, 0) ==
		    (ssize_t)(sizeof(target_payload) - 1U));
		CHECK(memcmp(actual, target_payload,
		    sizeof(target_payload) - 1U) == 0);
		CHECK(mount_free_blocks(&mountp) == free_before);

		inode_release(target);
		CHECK_ERROR(host_file_close(&target_handle), 0);
		CHECK(mount_free_blocks(&mountp) == free_before + 1U);
		inode_release(source);
		CHECK_ERROR(unlink_child(mountp.m_root, target_name), 0);
		CHECK(mount_free_blocks(&mountp) == free_initial);
		host_unmount(&mountp);
		check_fat_copies(&image);
		destroy_image(&image);
	}
}

static void
check_fat12_16_insert_slot_failures(void)
{
	static const enum bootfat_type types[] = {
		ZEDBSD_FAT12, ZEDBSD_FAT16,
	};
	static const char create_name[] = "INSERT.TXT";
	static const char source_name[] = "INSRC.TXT";
	static const char target_name[] = "INDST.TXT";
	static const char payload[] = "insert-slot rename";
	unsigned type_index;

	for (type_index = 0; type_index < ARRAY_COUNT(types); type_index++) {
		struct memory_image image;
		struct memory_image snapshot;
		struct mount mountp;
		struct mount snapshot_mount;
		struct inode *inode;
		struct inode *again;
		struct inode *absent = NULL;
		struct disk *disk;
		uint64_t identity;
		uint64_t found_identity;
		uint64_t free_initial;
		uint64_t free_before;
		char actual[sizeof(payload) - 1U];

		current_type = types[type_index];
		current_stage = "FAT12/16 create insert-slot write failure";
		current_fault_ordinal = 1U;
		format_image(&image, current_type, 1U, 2U);
		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		free_initial = mount_free_blocks(&mountp);

		image.fail_writes = 1U;
		CHECK_ERROR(create_child(mountp.m_root, create_name, &inode), EIO);
		CHECK(image.fail_writes == 0U);
		absent = NULL;
		CHECK_ERROR(lookup_child(mountp.m_root, create_name, &absent),
		    ENOENT);
		CHECK(mount_free_blocks(&mountp) == free_initial);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
		clone_image(&image, &snapshot);
		CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY,
		    &snapshot_mount), 0);
		absent = NULL;
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, create_name,
		    &absent), ENOENT);
		CHECK(mount_free_blocks(&snapshot_mount) == free_initial);
		host_unmount(&snapshot_mount);
		destroy_image(&snapshot);
		CHECK_ERROR(create_child(mountp.m_root, create_name, &inode), 0);
		CHECK(inode->i_size == 0);
		inode_release(inode);
		CHECK_ERROR(unlink_child(mountp.m_root, create_name), 0);
		CHECK(mount_free_blocks(&mountp) == free_initial);

		current_stage = "FAT12/16 rename insert-slot write failure";
		inode = create_payload(mountp.m_root, source_name, payload,
		    sizeof(payload) - 1U);
		CHECK_ERROR(fat_file_backing_identity(inode, &disk, &identity), 0);
		CHECK(disk == &image.disk);
		free_before = mount_free_blocks(&mountp);
		CHECK(free_before == free_initial - 1U);
		image.fail_writes = 1U;
		CHECK_ERROR(rename_child(mountp.m_root, source_name,
		    mountp.m_root, target_name), EIO);
		CHECK(image.fail_writes == 0U);
		CHECK_ERROR(lookup_child(mountp.m_root, source_name, &again), 0);
		CHECK(again == inode);
		CHECK_ERROR(fat_file_backing_identity(again, &disk,
		    &found_identity), 0);
		CHECK(disk == &image.disk);
		CHECK(found_identity == identity);
		read_inode(again, actual, sizeof(actual));
		CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
		inode_release(again);
		absent = NULL;
		CHECK_ERROR(lookup_child(mountp.m_root, target_name, &absent),
		    ENOENT);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);

		clone_image(&image, &snapshot);
		CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY,
		    &snapshot_mount), 0);
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, source_name,
		    &again), 0);
		CHECK_ERROR(fat_file_backing_identity(again, &disk,
		    &found_identity), 0);
		CHECK(disk == &snapshot.disk);
		CHECK(found_identity == identity);
		inode_release(again);
		read_path(&snapshot_mount, source_name, payload,
		    sizeof(payload) - 1U);
		absent = NULL;
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, target_name,
		    &absent), ENOENT);
		CHECK(mount_free_blocks(&snapshot_mount) == free_before);
		host_unmount(&snapshot_mount);
		destroy_image(&snapshot);

		CHECK_ERROR(rename_child(mountp.m_root, source_name,
		    mountp.m_root, target_name), 0);
		absent = NULL;
		CHECK_ERROR(lookup_child(mountp.m_root, source_name, &absent),
		    ENOENT);
		CHECK_ERROR(lookup_child(mountp.m_root, target_name, &again), 0);
		CHECK(again == inode);
		read_inode(again, actual, sizeof(actual));
		CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
		inode_release(again);
		inode_release(inode);
		CHECK_ERROR(unlink_child(mountp.m_root, target_name), 0);
		CHECK(mount_free_blocks(&mountp) == free_initial);
		host_unmount(&mountp);
		check_fat_copies(&image);
		destroy_image(&image);
	}
}

static void
check_read_only(struct memory_image *image, int disk_read_only)
{
	struct mount mountp;
	struct inode *hello;
	struct inode *created;
	struct file file;
	char byte;
	unsigned flags = disk_read_only ? 0U : MOUNT_READ_ONLY;

	if (disk_read_only)
		image->disk.d_flags |= DISK_READ_ONLY;
	CHECK_ERROR(host_mount(image, flags, &mountp), 0);
	CHECK_ERROR(lookup_child(mountp.m_root, "HELLO.TXT", &hello), 0);
	CHECK_ERROR(host_file_open(hello, O_RDONLY, &file), 0);
	CHECK(file.f_ops->read(&file, &byte, 1U) == 1);
	CHECK_ERROR(host_file_close(&file), 0);
	CHECK_ERROR(create_child(mountp.m_root, "RO.TXT", &created), EROFS);
	CHECK_ERROR(mkdir_child(mountp.m_root, "RODIR", &created), EROFS);
	CHECK_ERROR(unlink_child(mountp.m_root, "HELLO.TXT"), EROFS);
	CHECK_ERROR(rename_child(mountp.m_root, "HELLO.TXT", mountp.m_root,
	    "OTHER.TXT"), EROFS);
	CHECK_ERROR(hello->i_op->truncate(hello, 0), EROFS);
	CHECK_ERROR(host_file_open(hello, O_RDWR, &file), 0);
	CHECK(file.f_ops->pwrite(&file, "x", 1U, 0) == -EROFS);
	CHECK_ERROR(file.f_ops->fsync(&file), EROFS);
	CHECK_ERROR(host_file_close(&file), EROFS);
	inode_release(hello);
	host_unmount(&mountp);
	image->disk.d_flags &= ~DISK_READ_ONLY;
}

static void
mark_all_clusters_allocated(struct memory_image *image)
{
	uint32_t cluster;
	uint32_t eoc = fat_end_of_chain(image->type);

	for (cluster = 2U; cluster < image->cluster_count + 2U; cluster++)
		set_fat_entry(image, cluster, eoc);
	check_fat_copies(image);
}

static void
check_no_space(enum bootfat_type type)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *full;
	struct file file;

	format_image(&image, type, 1U, 2U);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	CHECK_ERROR(create_child(mountp.m_root, "FULL.TXT", &full), 0);
	CHECK_ERROR(host_file_open(full, O_RDWR, &file), 0);
	CHECK_ERROR(file.f_ops->fsync(&file), 0);
	CHECK_ERROR(host_file_close(&file), 0);
	inode_release(full);
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	host_unmount(&mountp);

	mark_all_clusters_allocated(&image);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	CHECK(mount_free_blocks(&mountp) == 0U);
	CHECK_ERROR(lookup_child(mountp.m_root, "FULL.TXT", &full), 0);
	CHECK_ERROR(host_file_open(full, O_RDWR, &file), 0);
	CHECK(file.f_ops->pwrite(&file, "x", 1U, 0) == -ENOSPC);
	CHECK(full->i_size == 0);
	CHECK_ERROR(host_file_close(&file), 0);
	inode_release(full);
	host_unmount(&mountp);
	destroy_image(&image);
}

static void
schedule_write_failure(struct memory_image *image, unsigned ordinal)
{
	CHECK(ordinal != 0U);
	image->fail_write_attempt = image->write_attempts + ordinal;
}

static void
schedule_two_write_failures(struct memory_image *image, unsigned first,
	unsigned second)
{
	CHECK(first != 0U);
	CHECK(second > first);
	image->fail_write_attempt = image->write_attempts + first;
	image->fail_write_attempt2 = image->write_attempts + second;
}

static void
prepare_dotdot_rename_image(struct memory_image *image,
	uint64_t *free_before, uint32_t *old_parent_cluster,
	uint32_t *new_parent_cluster, uint32_t *moved_cluster)
{
	static const uint8_t a_sfn[11] = {
		'A', '~', '1', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
	};
	static const uint8_t b_sfn[11] = {
		'B', '~', '1', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
	};
	static const uint8_t move_sfn[11] = {
		'M', 'O', 'V', 'E', '~', '1', ' ', ' ', ' ', ' ', ' ',
	};
	struct mount mountp;
	struct inode *a;
	struct inode *b;
	struct inode *moved;
	struct inode *parent;

	format_image(image, ZEDBSD_FAT32, 1U, 2U);
	CHECK_ERROR(host_mount(image, 0U, &mountp), 0);
	CHECK_ERROR(mkdir_child(mountp.m_root, "A", &a), 0);
	CHECK_ERROR(mkdir_child(mountp.m_root, "B", &b), 0);
	CHECK_ERROR(mkdir_child(a, "MOVE", &moved), 0);
	CHECK_ERROR(lookup_child(moved, "..", &parent), 0);
	CHECK(parent == a);
	inode_release(parent);
	*free_before = mount_free_blocks(&mountp);
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	inode_release(moved);
	inode_release(a);
	inode_release(b);
	host_unmount(&mountp);
	check_fat_copies(image);

	*old_parent_cluster = raw_find_sfn_cluster(image,
	    image->root_cluster, a_sfn);
	*new_parent_cluster = raw_find_sfn_cluster(image,
	    image->root_cluster, b_sfn);
	*moved_cluster = raw_find_sfn_cluster(image, *old_parent_cluster,
	    move_sfn);
	CHECK(*old_parent_cluster != *new_parent_cluster);
	CHECK(*moved_cluster != *old_parent_cluster);
	CHECK(*moved_cluster != *new_parent_cluster);
	CHECK(raw_dotdot_parent_cluster(image, *moved_cluster) ==
	    *old_parent_cluster);
}

static void
check_dotdot_rename_faults(void)
{
	struct memory_image image;
	struct memory_image snapshot;
	struct mount mountp;
	struct mount snapshot_mount;
	struct inode *a;
	struct inode *b;
	struct inode *moved;
	struct inode *again;
	struct inode *parent;
	struct inode *absent;
	uint8_t *checkpoint;
	uint64_t free_before;
	uint32_t old_parent_cluster;
	uint32_t new_parent_cluster;
	uint32_t moved_cluster;
	unsigned ordinal;
	unsigned consumed = 0;

	current_type = ZEDBSD_FAT32;
	current_stage = "cross-directory dotdot rename setup";
	current_fault_ordinal = 0;
	prepare_dotdot_rename_image(&image, &free_before,
	    &old_parent_cluster, &new_parent_cluster, &moved_cluster);
	checkpoint = malloc((size_t)image.sectors * SECTOR_SIZE);
	CHECK(checkpoint != NULL);
	memcpy(checkpoint, image.bytes, (size_t)image.sectors * SECTOR_SIZE);

	for (ordinal = 1U; ordinal <= 16U; ordinal++) {
		int rename_error;

		current_stage = "cross-directory dotdot rename rollback";
		current_fault_ordinal = ordinal;
		memcpy(image.bytes, checkpoint,
		    (size_t)image.sectors * SECTOR_SIZE);
		image.fail_write_attempt = 0U;
		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(lookup_child(mountp.m_root, "A", &a), 0);
		CHECK_ERROR(lookup_child(mountp.m_root, "B", &b), 0);
		CHECK_ERROR(lookup_child(a, "MOVE", &moved), 0);
		schedule_write_failure(&image, ordinal);
		rename_error = rename_child(a, "MOVE", b, "MOVED");
		if (image.fail_write_attempt != 0U) {
			CHECK_ERROR(rename_error, 0);
			image.fail_write_attempt = 0U;
			absent = NULL;
			CHECK_ERROR(lookup_child(a, "MOVE", &absent), ENOENT);
			CHECK_ERROR(lookup_child(b, "MOVED", &again), 0);
			CHECK(again == moved);
			inode_release(again);
			CHECK_ERROR(lookup_child(moved, "..", &parent), 0);
			CHECK(parent == b);
			inode_release(parent);
			CHECK(raw_dotdot_parent_cluster(&image, moved_cluster) ==
			    new_parent_cluster);
			CHECK(mount_free_blocks(&mountp) == free_before);
			inode_release(moved);
			inode_release(a);
			inode_release(b);
			host_unmount(&mountp);
			break;
		}

		CHECK_ERROR(rename_error, EIO);
		consumed++;
		CHECK_ERROR(lookup_child(a, "MOVE", &again), 0);
		CHECK(again == moved);
		inode_release(again);
		absent = NULL;
		CHECK_ERROR(lookup_child(b, "MOVED", &absent), ENOENT);
		CHECK_ERROR(lookup_child(moved, "..", &parent), 0);
		CHECK(parent == a);
		inode_release(parent);
		CHECK(raw_dotdot_parent_cluster(&image, moved_cluster) ==
		    old_parent_cluster);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
		CHECK(raw_dotdot_parent_cluster(&image, moved_cluster) ==
		    old_parent_cluster);

		clone_image(&image, &snapshot);
		CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY,
		    &snapshot_mount), 0);
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, "A", &again), 0);
		CHECK_ERROR(lookup_child(again, "MOVE", &parent), 0);
		CHECK(raw_dotdot_parent_cluster(&snapshot, moved_cluster) ==
		    old_parent_cluster);
		CHECK_ERROR(lookup_child(parent, "..", &absent), 0);
		CHECK(absent == again);
		inode_release(absent);
		inode_release(parent);
		inode_release(again);
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, "B", &again), 0);
		absent = NULL;
		CHECK_ERROR(lookup_child(again, "MOVED", &absent), ENOENT);
		inode_release(again);
		CHECK(mount_free_blocks(&snapshot_mount) == free_before);
		host_unmount(&snapshot_mount);
		destroy_image(&snapshot);

		CHECK_ERROR(rename_child(a, "MOVE", b, "MOVED"), 0);
		absent = NULL;
		CHECK_ERROR(lookup_child(a, "MOVE", &absent), ENOENT);
		CHECK_ERROR(lookup_child(b, "MOVED", &again), 0);
		CHECK(again == moved);
		inode_release(again);
		CHECK_ERROR(lookup_child(moved, "..", &parent), 0);
		CHECK(parent == b);
		inode_release(parent);
		CHECK(raw_dotdot_parent_cluster(&image, moved_cluster) ==
		    new_parent_cluster);
		CHECK(mount_free_blocks(&mountp) == free_before);
		inode_release(moved);
		inode_release(a);
		inode_release(b);
		host_unmount(&mountp);
	}
	CHECK(consumed == 5U);
	CHECK(ordinal == 6U);
	free(checkpoint);
	destroy_image(&image);
}

static void
prepare_sector_boundary_lfn(struct memory_image *image, const char *name,
	const void *payload, size_t payload_size, uint64_t *free_before,
	uint64_t *identity)
{
	static const char *const fillers[] = {
		"ABCDEFGHIJ.TXT", "F1.TXT", "F2.TXT", "F3.TXT",
	};
	struct mount mountp;
	struct inode *inode;
	struct disk *disk;
	const uint8_t *first_sector;
	const uint8_t *second_sector;
	uint32_t second_cluster;
	unsigned i;

	CHECK(strlen(name) > 26U && strlen(name) <= 39U);
	format_image(image, ZEDBSD_FAT32, 1U, 2U);
	CHECK_ERROR(host_mount(image, 0U, &mountp), 0);
	for (i = 0; i < ARRAY_COUNT(fillers); i++) {
		CHECK_ERROR(create_child(mountp.m_root, fillers[i], &inode), 0);
		inode_release(inode);
	}
	inode = create_payload(mountp.m_root, name, payload, payload_size);
	CHECK_ERROR(fat_file_backing_identity(inode, &disk, identity), 0);
	CHECK(disk == &image->disk);
	inode_release(inode);
	*free_before = mount_free_blocks(&mountp);
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	host_unmount(&mountp);
	check_fat_copies(image);

	/* Initial entries occupy indices 0..3.  The 3+2+2+2 filler runs occupy
	 * 4..12, so this target's three LFNs are 13..15 and its SFN is index 16,
	 * offset zero in the next directory sector/cluster. */
	second_cluster = get_fat_entry(image, 0U, image->root_cluster);
	CHECK(second_cluster == 6U);
	CHECK(get_fat_entry(image, 1U, image->root_cluster) == second_cluster);
	CHECK((*identity & 0xffffU) == 0U);
	CHECK((*identity >> 16) == cluster_lba(image, second_cluster));
	first_sector = image_sector(image,
	    cluster_lba(image, image->root_cluster));
	second_sector = image_sector(image, cluster_lba(image, second_cluster));
	CHECK(first_sector[12U * 32U + 11U] == 0x20U);
	CHECK(first_sector[13U * 32U + 11U] == 0x0fU);
	CHECK(first_sector[14U * 32U + 11U] == 0x0fU);
	CHECK(first_sector[15U * 32U + 11U] == 0x0fU);
	CHECK(second_sector[11U] == 0x20U);
}

static void
check_sector_boundary_lfn_unlink_faults(void)
{
	static const char name[] = "Boundary Sector Long File Name.txt";
	static const char payload[] = "boundary unlink payload";
	struct memory_image image;
	struct memory_image snapshot;
	struct mount mountp;
	struct mount snapshot_mount;
	struct inode *inode;
	struct inode *again;
	struct inode *absent;
	struct file file;
	struct disk *disk;
	uint8_t actual[sizeof(payload) - 1U];
	uint8_t *checkpoint;
	uint64_t free_before;
	uint64_t identity;
	uint64_t found_identity;
	unsigned ordinal;
	unsigned consumed = 0;

	current_type = ZEDBSD_FAT32;
	current_stage = "sector-boundary LFN unlink setup";
	current_fault_ordinal = 0;
	prepare_sector_boundary_lfn(&image, name, payload,
	    sizeof(payload) - 1U, &free_before, &identity);
	checkpoint = malloc((size_t)image.sectors * SECTOR_SIZE);
	CHECK(checkpoint != NULL);
	memcpy(checkpoint, image.bytes, (size_t)image.sectors * SECTOR_SIZE);

	for (ordinal = 1U; ordinal <= 16U; ordinal++) {
		int unlink_error;

		current_stage = "sector-boundary LFN unlink rollback";
		current_fault_ordinal = ordinal;
		memcpy(image.bytes, checkpoint,
		    (size_t)image.sectors * SECTOR_SIZE);
		image.fail_write_attempt = 0U;
		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(lookup_child(mountp.m_root, name, &inode), 0);
		CHECK_ERROR(fat_file_backing_identity(inode, &disk,
		    &found_identity), 0);
		CHECK(disk == &image.disk);
		CHECK(found_identity == identity);
		CHECK_ERROR(host_file_open(inode, O_RDONLY, &file), 0);
		schedule_write_failure(&image, ordinal);
		unlink_error = unlink_child(mountp.m_root, name);
		if (image.fail_write_attempt != 0U) {
			CHECK_ERROR(unlink_error, 0);
			image.fail_write_attempt = 0U;
			absent = NULL;
			CHECK_ERROR(lookup_child(mountp.m_root, name, &absent),
			    ENOENT);
			inode_release(inode);
			CHECK_ERROR(host_file_close(&file), 0);
			CHECK(mount_free_blocks(&mountp) == free_before + 1U);
			host_unmount(&mountp);
			break;
		}

		CHECK_ERROR(unlink_error, EIO);
		consumed++;
		CHECK_ERROR(lookup_child(mountp.m_root, name, &again), 0);
		CHECK(again == inode);
		inode_release(again);
		CHECK(file.f_ops->pread(&file, actual, sizeof(actual), 0) ==
		    (ssize_t)sizeof(actual));
		CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);

		clone_image(&image, &snapshot);
		CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY,
		    &snapshot_mount), 0);
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, name, &again), 0);
		CHECK_ERROR(fat_file_backing_identity(again, &disk,
		    &found_identity), 0);
		CHECK(disk == &snapshot.disk);
		CHECK(found_identity == identity);
		inode_release(again);
		read_path(&snapshot_mount, name, payload, sizeof(payload) - 1U);
		CHECK(mount_free_blocks(&snapshot_mount) == free_before);
		host_unmount(&snapshot_mount);
		destroy_image(&snapshot);

		CHECK_ERROR(unlink_child(mountp.m_root, name), 0);
		absent = NULL;
		CHECK_ERROR(lookup_child(mountp.m_root, name, &absent), ENOENT);
		CHECK(file.f_ops->pread(&file, actual, sizeof(actual), 0) ==
		    (ssize_t)sizeof(actual));
		CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
		inode_release(inode);
		CHECK_ERROR(host_file_close(&file), 0);
		CHECK(mount_free_blocks(&mountp) == free_before + 1U);
		host_unmount(&mountp);
	}
	CHECK(consumed == 4U);
	CHECK(ordinal == 5U);
	free(checkpoint);
	destroy_image(&image);
}

static void
check_sector_boundary_lfn_rename_faults(void)
{
	static const char old_name[] = "Boundary Sector Long File Name.txt";
	static const char new_name[] = "Boundary Rename Destination Name.txt";
	static const char payload[] = "boundary rename payload";
	struct memory_image image;
	struct memory_image snapshot;
	struct mount mountp;
	struct mount snapshot_mount;
	struct inode *inode;
	struct inode *again;
	struct inode *absent;
	struct disk *disk;
	uint8_t actual[sizeof(payload) - 1U];
	uint8_t *checkpoint;
	uint64_t free_before;
	uint64_t identity;
	uint64_t found_identity;
	unsigned ordinal;
	unsigned consumed = 0;

	current_type = ZEDBSD_FAT32;
	current_stage = "sector-boundary LFN rename setup";
	current_fault_ordinal = 0;
	prepare_sector_boundary_lfn(&image, old_name, payload,
	    sizeof(payload) - 1U, &free_before, &identity);
	checkpoint = malloc((size_t)image.sectors * SECTOR_SIZE);
	CHECK(checkpoint != NULL);
	memcpy(checkpoint, image.bytes, (size_t)image.sectors * SECTOR_SIZE);

	for (ordinal = 1U; ordinal <= 24U; ordinal++) {
		int rename_error;

		current_stage = "sector-boundary LFN rename rollback";
		current_fault_ordinal = ordinal;
		memcpy(image.bytes, checkpoint,
		    (size_t)image.sectors * SECTOR_SIZE);
		image.fail_write_attempt = 0U;
		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(lookup_child(mountp.m_root, old_name, &inode), 0);
		CHECK_ERROR(fat_file_backing_identity(inode, &disk,
		    &found_identity), 0);
		CHECK(disk == &image.disk);
		CHECK(found_identity == identity);
		schedule_write_failure(&image, ordinal);
		rename_error = rename_child(mountp.m_root, old_name,
		    mountp.m_root, new_name);
		if (image.fail_write_attempt != 0U) {
			CHECK_ERROR(rename_error, 0);
			image.fail_write_attempt = 0U;
			absent = NULL;
			CHECK_ERROR(lookup_child(mountp.m_root, old_name, &absent),
			    ENOENT);
			CHECK_ERROR(lookup_child(mountp.m_root, new_name, &again), 0);
			CHECK(again == inode);
			read_inode(again, actual, sizeof(actual));
			CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
			inode_release(again);
			inode_release(inode);
			CHECK_ERROR(unlink_child(mountp.m_root, new_name), 0);
			CHECK(mount_free_blocks(&mountp) == free_before + 1U);
			host_unmount(&mountp);
			break;
		}

		CHECK_ERROR(rename_error, EIO);
		consumed++;
		CHECK_ERROR(lookup_child(mountp.m_root, old_name, &again), 0);
		CHECK(again == inode);
		CHECK_ERROR(fat_file_backing_identity(again, &disk,
		    &found_identity), 0);
		CHECK(disk == &image.disk);
		CHECK(found_identity == identity);
		inode_release(again);
		absent = NULL;
		CHECK_ERROR(lookup_child(mountp.m_root, new_name, &absent), ENOENT);
		read_inode(inode, actual, sizeof(actual));
		CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);

		clone_image(&image, &snapshot);
		CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY,
		    &snapshot_mount), 0);
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, old_name, &again), 0);
		CHECK_ERROR(fat_file_backing_identity(again, &disk,
		    &found_identity), 0);
		CHECK(disk == &snapshot.disk);
		CHECK(found_identity == identity);
		inode_release(again);
		read_path(&snapshot_mount, old_name, payload,
		    sizeof(payload) - 1U);
		absent = NULL;
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, new_name, &absent),
		    ENOENT);
		CHECK(mount_free_blocks(&snapshot_mount) == free_before);
		host_unmount(&snapshot_mount);
		destroy_image(&snapshot);

		CHECK_ERROR(rename_child(mountp.m_root, old_name, mountp.m_root,
		    new_name), 0);
		absent = NULL;
		CHECK_ERROR(lookup_child(mountp.m_root, old_name, &absent), ENOENT);
		CHECK_ERROR(lookup_child(mountp.m_root, new_name, &again), 0);
		CHECK(again == inode);
		read_inode(again, actual, sizeof(actual));
		CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
		inode_release(again);
		inode_release(inode);
		CHECK_ERROR(unlink_child(mountp.m_root, new_name), 0);
		CHECK(mount_free_blocks(&mountp) == free_before + 1U);
		host_unmount(&mountp);
	}
	CHECK(consumed == 8U);
	CHECK(ordinal == 9U);
	free(checkpoint);
	destroy_image(&image);
}

static void
check_sector_boundary_lfn_faults(void)
{
	check_sector_boundary_lfn_unlink_faults();
	check_sector_boundary_lfn_rename_faults();
}

static void
check_maximum_lfn_name(void)
{
	static const char renamed[] = "MAXREN.TXT";
	static const char payload[] = "maximum LFN payload";
	struct memory_image image;
	struct mount mountp;
	struct inode *inode;
	struct inode *again;
	struct inode *absent = NULL;
	struct file directory;
	struct dirent entry;
	char maximum[NAME_MAX + 1U];
	char too_long[NAME_MAX + 2U];
	uint64_t free_initial;
	uint64_t free_after_create;
	unsigned writes_before;
	int eof = 0;
	int saw_maximum = 0;

	current_type = ZEDBSD_FAT32;
	current_stage = "maximum FAT32 LFN basename";
	current_fault_ordinal = 0;
	memset(maximum, 'm', NAME_MAX);
	maximum[NAME_MAX] = '\0';
	memset(too_long, 'n', NAME_MAX + 1U);
	too_long[NAME_MAX + 1U] = '\0';
	CHECK(strlen(maximum) == NAME_MAX);
	CHECK(strlen(too_long) == NAME_MAX + 1U);
	format_image(&image, ZEDBSD_FAT32, 1U, 2U);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	free_initial = mount_free_blocks(&mountp);

	writes_before = image.writes;
	CHECK_ERROR(create_child(mountp.m_root, too_long, &inode),
	    ENAMETOOLONG);
	CHECK(image.writes == writes_before);
	CHECK(mount_free_blocks(&mountp) == free_initial);
	absent = NULL;
	CHECK_ERROR(lookup_child(mountp.m_root, too_long, &absent),
	    ENAMETOOLONG);

	inode = create_payload(mountp.m_root, maximum, payload,
	    sizeof(payload) - 1U);
	CHECK_ERROR(lookup_child(mountp.m_root, maximum, &again), 0);
	CHECK(again == inode);
	inode_release(again);
	free_after_create = mount_free_blocks(&mountp);
	CHECK(free_after_create == free_initial - 2U);
	CHECK_ERROR(host_file_open(mountp.m_root, O_RDONLY | O_DIRECTORY,
	    &directory), 0);
	while (!eof) {
		CHECK_ERROR(directory.f_ops->readdir(&directory, &entry, &eof), 0);
		if (!eof && strcmp(entry.d_name, maximum) == 0)
			saw_maximum = 1;
	}
	CHECK(saw_maximum);
	CHECK_ERROR(host_file_close(&directory), 0);
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	inode_release(inode);
	host_unmount(&mountp);
	check_fat_copies(&image);

	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	CHECK_ERROR(lookup_child(mountp.m_root, maximum, &inode), 0);
	read_path(&mountp, maximum, payload, sizeof(payload) - 1U);
	CHECK(mount_free_blocks(&mountp) == free_after_create);
	CHECK_ERROR(rename_child(mountp.m_root, maximum, mountp.m_root,
	    renamed), 0);
	absent = NULL;
	CHECK_ERROR(lookup_child(mountp.m_root, maximum, &absent), ENOENT);
	CHECK_ERROR(lookup_child(mountp.m_root, renamed, &again), 0);
	CHECK(again == inode);
	read_inode(again, too_long, sizeof(payload) - 1U);
	CHECK(memcmp(too_long, payload, sizeof(payload) - 1U) == 0);
	inode_release(again);
	inode_release(inode);
	CHECK_ERROR(unlink_child(mountp.m_root, renamed), 0);
	CHECK(mount_free_blocks(&mountp) == free_initial - 1U);
	writes_before = image.writes;
	CHECK_ERROR(create_child(mountp.m_root, too_long, &inode),
	    ENAMETOOLONG);
	CHECK(image.writes == writes_before);
	CHECK(mount_free_blocks(&mountp) == free_initial - 1U);
	host_unmount(&mountp);
	destroy_image(&image);
}

static void
check_lfn_unlink_faults(void)
{
	static const char name[] = "Unlink Atomicity Long File Name.txt";
	static const char payload[] = "unlink payload";
	struct memory_image image;
	struct memory_image snapshot;
	struct mount mountp;
	struct mount snapshot_mount;
	struct inode *inode;
	struct inode *again;
	struct inode *absent;
	struct file file;
	uint8_t actual[sizeof(payload) - 1U];
	uint8_t *checkpoint;
	uint64_t free_before;
	unsigned ordinal;
	unsigned consumed = 0;

	current_type = ZEDBSD_FAT32;
	current_stage = "LFN unlink fault setup";
	current_fault_ordinal = 0;
	format_image(&image, ZEDBSD_FAT32, 1U, 2U);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	inode = create_payload(mountp.m_root, name, payload,
	    sizeof(payload) - 1U);
	inode_release(inode);
	free_before = mount_free_blocks(&mountp);
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	host_unmount(&mountp);
	checkpoint = malloc((size_t)image.sectors * SECTOR_SIZE);
	CHECK(checkpoint != NULL);
	memcpy(checkpoint, image.bytes, (size_t)image.sectors * SECTOR_SIZE);

	for (ordinal = 1U; ordinal <= 32U; ordinal++) {
		int unlink_error;

		current_stage = "LFN unlink write rollback";
		current_fault_ordinal = ordinal;
		memcpy(image.bytes, checkpoint,
		    (size_t)image.sectors * SECTOR_SIZE);
		image.fail_write_attempt = 0U;
		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(lookup_child(mountp.m_root, name, &inode), 0);
		CHECK_ERROR(host_file_open(inode, O_RDONLY, &file), 0);
		schedule_write_failure(&image, ordinal);
		unlink_error = unlink_child(mountp.m_root, name);
		if (image.fail_write_attempt != 0U) {
			CHECK_ERROR(unlink_error, 0);
			image.fail_write_attempt = 0U;
			absent = NULL;
			CHECK_ERROR(lookup_child(mountp.m_root, name, &absent),
			    ENOENT);
			inode_release(inode);
			CHECK_ERROR(host_file_close(&file), 0);
			CHECK(mount_free_blocks(&mountp) == free_before + 1U);
			host_unmount(&mountp);
			break;
		}

		CHECK_ERROR(unlink_error, EIO);
		consumed++;
		CHECK(image.fail_write_attempt == 0U);
		CHECK_ERROR(lookup_child(mountp.m_root, name, &again), 0);
		CHECK(again == inode);
		inode_release(again);
		CHECK(file.f_ops->pread(&file, actual, sizeof(actual), 0) ==
		    (ssize_t)sizeof(actual));
		CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);

		clone_image(&image, &snapshot);
		CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY,
		    &snapshot_mount), 0);
		read_path(&snapshot_mount, name, payload, sizeof(payload) - 1U);
		CHECK(mount_free_blocks(&snapshot_mount) == free_before);
		host_unmount(&snapshot_mount);
		destroy_image(&snapshot);

		CHECK_ERROR(unlink_child(mountp.m_root, name), 0);
		absent = NULL;
		CHECK_ERROR(lookup_child(mountp.m_root, name, &absent), ENOENT);
		CHECK(file.f_ops->pread(&file, actual, sizeof(actual), 0) ==
		    (ssize_t)sizeof(actual));
		CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
		inode_release(inode);
		CHECK_ERROR(host_file_close(&file), 0);
		CHECK(mount_free_blocks(&mountp) == free_before + 1U);
		host_unmount(&mountp);
	}
	CHECK(consumed >= 2U);
	CHECK(ordinal <= 32U);
	free(checkpoint);
	destroy_image(&image);
}

static void
check_lfn_rename_faults(void)
{
	static const char old_name[] =
	    "Rename Atomicity Original Long Name.txt";
	static const char new_name[] =
	    "Rename Atomicity Destination Long Name.txt";
	static const char payload[] = "rename payload";
	struct memory_image image;
	struct memory_image snapshot;
	struct mount mountp;
	struct mount snapshot_mount;
	struct inode *inode;
	struct inode *again;
	struct inode *absent;
	uint8_t actual[sizeof(payload) - 1U];
	uint8_t *checkpoint;
	uint64_t free_before;
	unsigned ordinal;
	unsigned consumed = 0;

	current_type = ZEDBSD_FAT32;
	current_stage = "LFN rename fault setup";
	current_fault_ordinal = 0;
	format_image(&image, ZEDBSD_FAT32, 1U, 2U);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	inode = create_payload(mountp.m_root, old_name, payload,
	    sizeof(payload) - 1U);
	inode_release(inode);
	free_before = mount_free_blocks(&mountp);
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	host_unmount(&mountp);
	checkpoint = malloc((size_t)image.sectors * SECTOR_SIZE);
	CHECK(checkpoint != NULL);
	memcpy(checkpoint, image.bytes, (size_t)image.sectors * SECTOR_SIZE);

	for (ordinal = 1U; ordinal <= 32U; ordinal++) {
		int rename_error;

		current_stage = "LFN rename write rollback";
		current_fault_ordinal = ordinal;
		memcpy(image.bytes, checkpoint,
		    (size_t)image.sectors * SECTOR_SIZE);
		image.fail_write_attempt = 0U;
		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(lookup_child(mountp.m_root, old_name, &inode), 0);
		schedule_write_failure(&image, ordinal);
		rename_error = rename_child(mountp.m_root, old_name,
		    mountp.m_root, new_name);
		if (image.fail_write_attempt != 0U) {
			CHECK_ERROR(rename_error, 0);
			image.fail_write_attempt = 0U;
			absent = NULL;
			CHECK_ERROR(lookup_child(mountp.m_root, old_name, &absent),
			    ENOENT);
			CHECK_ERROR(lookup_child(mountp.m_root, new_name, &again), 0);
			CHECK(again == inode);
			read_inode(again, actual, sizeof(actual));
			CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
			inode_release(again);
			inode_release(inode);
			CHECK_ERROR(unlink_child(mountp.m_root, new_name), 0);
			CHECK(mount_free_blocks(&mountp) == free_before + 1U);
			host_unmount(&mountp);
			break;
		}

		CHECK_ERROR(rename_error, EIO);
		consumed++;
		CHECK(image.fail_write_attempt == 0U);
		CHECK_ERROR(lookup_child(mountp.m_root, old_name, &again), 0);
		CHECK(again == inode);
		inode_release(again);
		absent = NULL;
		CHECK_ERROR(lookup_child(mountp.m_root, new_name, &absent), ENOENT);
		read_inode(inode, actual, sizeof(actual));
		CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);

		clone_image(&image, &snapshot);
		CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY,
		    &snapshot_mount), 0);
		read_path(&snapshot_mount, old_name, payload,
		    sizeof(payload) - 1U);
		absent = NULL;
		CHECK_ERROR(lookup_child(snapshot_mount.m_root, new_name, &absent),
		    ENOENT);
		CHECK(mount_free_blocks(&snapshot_mount) == free_before);
		host_unmount(&snapshot_mount);
		destroy_image(&snapshot);

		CHECK_ERROR(rename_child(mountp.m_root, old_name, mountp.m_root,
		    new_name), 0);
		absent = NULL;
		CHECK_ERROR(lookup_child(mountp.m_root, old_name, &absent), ENOENT);
		CHECK_ERROR(lookup_child(mountp.m_root, new_name, &again), 0);
		CHECK(again == inode);
		read_inode(again, actual, sizeof(actual));
		CHECK(memcmp(actual, payload, sizeof(actual)) == 0);
		inode_release(again);
		inode_release(inode);
		CHECK_ERROR(unlink_child(mountp.m_root, new_name), 0);
		CHECK(mount_free_blocks(&mountp) == free_before + 1U);
		host_unmount(&mountp);
	}
	CHECK(consumed >= 2U);
	CHECK(ordinal <= 32U);
	free(checkpoint);
	destroy_image(&image);
}

static void
check_lfn_namespace_faults(void)
{
	check_lfn_unlink_faults();
	check_lfn_rename_faults();
}

static void
verify_grown_file(struct memory_image *image, struct mount *mountp,
	uint64_t free_before)
{
	struct inode *inode;
	struct file file;
	struct extent_capture capture = {0};
	struct disk *disk;
	uint64_t block;
	char boundary[2];
	uint64_t free_after;

	free_after = mount_free_blocks(mountp);
	if (free_after != free_before - 1U)
		fprintf(stderr, "growth free blocks: got %llu, wanted %llu\n",
		    (unsigned long long)free_after,
		    (unsigned long long)(free_before - 1U));
	CHECK(free_after == free_before - 1U);
	CHECK_ERROR(lookup_child(mountp->m_root, "GROW.TXT", &inode), 0);
	CHECK(inode->i_size == 513);
	CHECK_ERROR(host_file_open(inode, O_RDONLY, &file), 0);
	CHECK(file.f_ops->pread(&file, boundary, sizeof(boundary), 511) == 2);
	CHECK(boundary[0] == 'G');
	CHECK(boundary[1] == 'X');
	CHECK_ERROR(fat_file_extents(&file, capture_extent, &capture), 0);
	CHECK(capture.used == 2U);
	CHECK(capture.file_block[0] == 0U);
	CHECK(capture.file_block[1] == 1U);
	CHECK(capture.count[0] == 1U);
	CHECK(capture.count[1] == 1U);
	CHECK(capture.disk_block[0] == cluster_lba(image, 2U));
	CHECK(capture.disk_block[1] != capture.disk_block[0] + 1U);
	CHECK_ERROR(fat_file_contiguous_block(&file, &disk, &block),
	    EOPNOTSUPP);
	CHECK_ERROR(host_file_close(&file), 0);
	inode_release(inode);
}

static void
check_partial_grow_faults(void)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *inode;
	struct file file;
	uint8_t payload[512];
	uint8_t *checkpoint;
	uint64_t free_before;
	unsigned ordinal;

	current_type = ZEDBSD_FAT16;
	current_stage = "partial cluster growth setup";
	memset(payload, 'G', sizeof(payload));
	format_image(&image, ZEDBSD_FAT16, 1U, 2U);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	inode = create_payload(mountp.m_root, "GROW.TXT", payload,
	    sizeof(payload));
	inode_release(inode);
	free_before = mount_free_blocks(&mountp);
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	host_unmount(&mountp);
	checkpoint = malloc((size_t)image.sectors * SECTOR_SIZE);
	CHECK(checkpoint != NULL);
	memcpy(checkpoint, image.bytes, (size_t)image.sectors * SECTOR_SIZE);

	/* FAT16 with two FAT copies performs six writes while adding the second
	 * cluster: zero, new EOC copies, old-tail link copies, and payload. */
	for (ordinal = 1U; ordinal <= 6U; ordinal++) {
		current_stage = "partial cluster growth retry";
		current_fault_ordinal = ordinal;
		memcpy(image.bytes, checkpoint,
		    (size_t)image.sectors * SECTOR_SIZE);
		image.fail_write_attempt = 0;
		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(lookup_child(mountp.m_root, "GROW.TXT", &inode), 0);
		CHECK_ERROR(host_file_open(inode, O_RDWR, &file), 0);
		schedule_write_failure(&image, ordinal);
		CHECK(file.f_ops->pwrite(&file, "X", 1U, 512) == -EIO);
		CHECK(image.fail_write_attempt == 0U);
		CHECK(inode->i_size == 512);
		CHECK(file.f_ops->pwrite(&file, "X", 1U, 512) == 1);
		CHECK(inode->i_size == 513);
		CHECK_ERROR(file.f_ops->fsync(&file), 0);
		CHECK_ERROR(host_file_close(&file), 0);
		inode_release(inode);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
		host_unmount(&mountp);
		check_fat_copies(&image);

		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		verify_grown_file(&image, &mountp, free_before);
		host_unmount(&mountp);
	}
	free(checkpoint);
	destroy_image(&image);
}

static void
verify_shrunk_file(struct mount *mountp, off_t wanted_size,
	uint64_t wanted_free, const uint8_t *expected)
{
	struct inode *inode;
	struct file file;
	struct extent_capture capture = {0};
	uint8_t buffer[512];
	uint64_t free_after;

	free_after = mount_free_blocks(mountp);
	if (free_after != wanted_free) {
		struct memory_image *image = mountp->m_disk->d_data;

		fprintf(stderr, "shrink free blocks: got %llu, wanted %llu\n",
		    (unsigned long long)free_after,
		    (unsigned long long)wanted_free);
		fprintf(stderr,
		    "shrink FAT chain: copy0 2=%#x 5=%#x 6=%#x; "
		    "copy1 2=%#x 5=%#x 6=%#x\n",
		    get_fat_entry(image, 0U, 2U),
		    get_fat_entry(image, 0U, 5U),
		    get_fat_entry(image, 0U, 6U),
		    get_fat_entry(image, 1U, 2U),
		    get_fat_entry(image, 1U, 5U),
		    get_fat_entry(image, 1U, 6U));
	}
	CHECK(free_after == wanted_free);
	CHECK_ERROR(lookup_child(mountp->m_root, "SHRINK.TXT", &inode), 0);
	CHECK(inode->i_size == wanted_size);
	CHECK_ERROR(host_file_open(inode, O_RDONLY, &file), 0);
	CHECK_ERROR(fat_file_extents(&file, capture_extent, &capture), 0);
	if (wanted_size == 0) {
		CHECK(capture.used == 0U);
		CHECK(file.f_ops->pread(&file, buffer, 1U, 0) == 0);
	} else {
		CHECK(capture.used == 1U);
		CHECK(capture.file_block[0] == 0U);
		CHECK(capture.count[0] == 1U);
		CHECK(file.f_ops->pread(&file, buffer, (size_t)wanted_size, 0) ==
		    wanted_size);
		CHECK(memcmp(buffer, expected, (size_t)wanted_size) == 0);
		CHECK(file.f_ops->pread(&file, buffer, 1U, wanted_size) == 0);
	}
	CHECK_ERROR(host_file_close(&file), 0);
	inode_release(inode);
}

static void
verify_failed_truncate_state(struct memory_image *image, struct mount *mountp,
	struct inode *inode, struct file *file, const uint8_t expected[1536],
	uint64_t free_before)
{
	struct memory_image snapshot;
	struct mount snapshot_mount;
	struct inode *snapshot_inode;
	struct file snapshot_file;
	struct extent_capture capture = {0};
	uint8_t buffer[1536];
	uint32_t covered = 0;
	unsigned i;

	CHECK(inode->i_size == (off_t)sizeof(buffer));
	CHECK(file->f_ops->pread(file, buffer, sizeof(buffer), 0) ==
	    (ssize_t)sizeof(buffer));
	CHECK(memcmp(buffer, expected, sizeof(buffer)) == 0);
	CHECK_ERROR(fat_file_extents(file, capture_extent, &capture), 0);
	for (i = 0; i < capture.used; i++) {
		CHECK(capture.file_block[i] == covered);
		covered += capture.count[i];
	}
	CHECK(covered == 3U);
	CHECK(mount_free_blocks(mountp) == free_before);
	CHECK_ERROR(mountp->m_type->sync(mountp), 0);
	check_fat_copies(image);

	clone_image(image, &snapshot);
	CHECK_ERROR(host_mount(&snapshot, MOUNT_READ_ONLY, &snapshot_mount), 0);
	CHECK(mount_free_blocks(&snapshot_mount) == free_before);
	CHECK_ERROR(lookup_child(snapshot_mount.m_root, "SHRINK.TXT",
	    &snapshot_inode), 0);
	CHECK(snapshot_inode->i_size == (off_t)sizeof(buffer));
	CHECK_ERROR(host_file_open(snapshot_inode, O_RDONLY, &snapshot_file), 0);
	CHECK(snapshot_file.f_ops->pread(&snapshot_file, buffer, sizeof(buffer),
	    0) == (ssize_t)sizeof(buffer));
	CHECK(memcmp(buffer, expected, sizeof(buffer)) == 0);
	memset(&capture, 0, sizeof(capture));
	covered = 0;
	CHECK_ERROR(fat_file_extents(&snapshot_file, capture_extent, &capture), 0);
	for (i = 0; i < capture.used; i++) {
		CHECK(capture.file_block[i] == covered);
		covered += capture.count[i];
	}
	CHECK(covered == 3U);
	CHECK_ERROR(host_file_close(&snapshot_file), 0);
	inode_release(snapshot_inode);
	host_unmount(&snapshot_mount);
	destroy_image(&snapshot);
}

static void
run_partial_shrink_sweep(struct memory_image *image, const uint8_t *checkpoint,
	const uint8_t expected[1536], uint64_t free_before, off_t wanted_size,
	unsigned released_clusters)
{
	struct mount mountp;
	struct inode *inode;
	struct file file;
	uint8_t buffer[512];
	unsigned ordinal;
	unsigned consumed = 0;

	/* Stop at the first ordinal beyond the implementation's successful write
	 * path.  This keeps the sweep complete if rollback ordering changes. */
	for (ordinal = 1U; ordinal <= 16U; ordinal++) {
		int truncate_error;

		current_stage = wanted_size == 0 ? "truncate-zero fault retry" :
		    "truncate-shrink fault retry";
		current_fault_ordinal = ordinal;
		memcpy(image->bytes, checkpoint,
		    (size_t)image->sectors * SECTOR_SIZE);
		image->fail_write_attempt = 0;
		CHECK_ERROR(host_mount(image, 0U, &mountp), 0);
		CHECK(mount_free_blocks(&mountp) == free_before);
		CHECK_ERROR(lookup_child(mountp.m_root, "SHRINK.TXT", &inode), 0);
		CHECK(inode->i_size == 1536);
		CHECK_ERROR(host_file_open(inode, O_RDWR, &file), 0);
		schedule_write_failure(image, ordinal);
		truncate_error = inode->i_op->truncate(inode, wanted_size);
		if (image->fail_write_attempt != 0U) {
			CHECK_ERROR(truncate_error, 0);
			image->fail_write_attempt = 0;
			CHECK_ERROR(host_file_close(&file), 0);
			inode_release(inode);
			host_unmount(&mountp);
			break;
		}
		CHECK_ERROR(truncate_error, EIO);
		consumed++;
		CHECK(image->fail_write_attempt == 0U);
		verify_failed_truncate_state(image, &mountp, inode, &file,
		    expected, free_before);
		CHECK_ERROR(inode->i_op->truncate(inode, wanted_size), 0);
		CHECK(inode->i_size == wanted_size);
		if (wanted_size == 0) {
			CHECK(file.f_ops->pread(&file, buffer, 1U, 0) == 0);
		} else {
			CHECK(file.f_ops->pread(&file, buffer,
			    (size_t)wanted_size, 0) == wanted_size);
			CHECK(memcmp(buffer, expected, (size_t)wanted_size) == 0);
		}
		CHECK_ERROR(file.f_ops->fsync(&file), 0);
		CHECK_ERROR(host_file_close(&file), 0);
		inode_release(inode);
		CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
		host_unmount(&mountp);
		check_fat_copies(image);

		CHECK_ERROR(host_mount(image, 0U, &mountp), 0);
		verify_shrunk_file(&mountp, wanted_size,
		    free_before + released_clusters,
		    expected);
		host_unmount(&mountp);
	}
	CHECK(consumed != 0U);
	CHECK(ordinal <= 16U);

	current_stage = wanted_size == 0 ? "truncate-zero journal allocation" :
	    "truncate-shrink journal allocation";
	current_fault_ordinal = 0;
	memcpy(image->bytes, checkpoint,
	    (size_t)image->sectors * SECTOR_SIZE);
	CHECK_ERROR(host_mount(image, 0U, &mountp), 0);
	CHECK_ERROR(lookup_child(mountp.m_root, "SHRINK.TXT", &inode), 0);
	CHECK_ERROR(host_file_open(inode, O_RDWR, &file), 0);
	fail_kern_allocations = 1U;
	CHECK_ERROR(inode->i_op->truncate(inode, wanted_size), ENOMEM);
	CHECK(fail_kern_allocations == 0U);
	verify_failed_truncate_state(image, &mountp, inode, &file, expected,
	    free_before);
	CHECK_ERROR(inode->i_op->truncate(inode, wanted_size), 0);
	CHECK_ERROR(file.f_ops->fsync(&file), 0);
	CHECK_ERROR(host_file_close(&file), 0);
	inode_release(inode);
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	host_unmount(&mountp);
	check_fat_copies(image);
	CHECK_ERROR(host_mount(image, 0U, &mountp), 0);
	verify_shrunk_file(&mountp, wanted_size,
	    free_before + released_clusters, expected);
	host_unmount(&mountp);
}

static void
check_partial_shrink_faults(void)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *inode;
	uint8_t payload[1536];
	uint8_t *checkpoint;
	uint64_t free_before;
	unsigned i;

	current_type = ZEDBSD_FAT16;
	current_stage = "partial truncate setup";
	for (i = 0; i < ARRAY_COUNT(payload); i++)
		payload[i] = (uint8_t)('a' + i % 26U);
	format_image(&image, ZEDBSD_FAT16, 1U, 2U);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	inode = create_payload(mountp.m_root, "SHRINK.TXT", payload,
	    sizeof(payload));
	inode_release(inode);
	free_before = mount_free_blocks(&mountp);
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	host_unmount(&mountp);
	checkpoint = malloc((size_t)image.sectors * SECTOR_SIZE);
	CHECK(checkpoint != NULL);
	memcpy(checkpoint, image.bytes, (size_t)image.sectors * SECTOR_SIZE);

	run_partial_shrink_sweep(&image, checkpoint, payload, free_before, 400,
	    2U);
	run_partial_shrink_sweep(&image, checkpoint, payload, free_before, 0,
	    3U);
	free(checkpoint);
	destroy_image(&image);
}

static void
check_mirrored_fat_rollback_failure_freezes_mount(void)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *created = NULL;

	current_type = ZEDBSD_FAT32;
	current_stage = "mirrored FAT rollback failure";
	current_fault_ordinal = 2U;
	format_image(&image, ZEDBSD_FAT32, 1U, 2U);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	/* mkdir first zeroes its new cluster, then updates the two FAT copies.
	 * Fail the second mirror and the dirty-cache flush at rollback entry so
	 * the mirrored allocation cannot be proven restored.
	 */
	schedule_two_write_failures(&image, 3U, 4U);
	CHECK_ERROR(mkdir_child(mountp.m_root, "MIRRORFAIL", &created), EIO);
	CHECK(created == NULL);
	CHECK(image.fail_write_attempt == 0U);
	CHECK(image.fail_write_attempt2 == 0U);
	CHECK(get_fat_entry(&image, 0U, 6U) !=
	    get_fat_entry(&image, 1U, 6U));
	/* A later mutation must not proceed on the uncertain allocation map. */
	CHECK_ERROR(create_child(mountp.m_root, "FROZEN.TXT", &created),
	    EROFS);
	CHECK(created == NULL);
	host_unmount(&mountp);
	destroy_image(&image);
}

static void
check_failed_create_restores_end_marker(void)
{
	static const uint8_t stale_sfn[11] = {
	    'S', 'T', 'A', 'L', 'E', ' ', ' ', ' ', 'T', 'X', 'T',
	};
	struct memory_image image;
	struct mount mountp;
	struct inode *created = NULL;
	uint8_t saved[4U * 32U];
	uint8_t *root;

	current_type = ZEDBSD_FAT32;
	current_stage = "failed create restores FAT end marker";
	current_fault_ordinal = 1U;
	format_image(&image, ZEDBSD_FAT32, 1U, 2U);
	root = image_sector(&image, cluster_lba(&image, image.root_cluster));
	/* The formatted image ends at slot 4.  A syntactically valid stale
	 * entry after that marker must remain hidden if a long-name create fails.
	 */
	CHECK(root[4U * 32U] == 0U);
	set_dirent(root + 7U * 32U, stale_sfn, 0x20U, 0U, 0U,
	    ZEDBSD_FAT32);
	memcpy(saved, root + 4U * 32U, sizeof(saved));
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	schedule_write_failure(&image, 2U);
	CHECK_ERROR(create_child(mountp.m_root, "Rollback End Marker.txt",
	    &created), EIO);
	CHECK(created == NULL);
	CHECK(image.fail_write_attempt == 0U);
	CHECK(memcmp(saved, root + 4U * 32U, sizeof(saved)) == 0);
	CHECK_ERROR(lookup_child(mountp.m_root, "Rollback End Marker.txt",
	    &created), ENOENT);
	CHECK_ERROR(lookup_child(mountp.m_root, "STALE.TXT", &created),
	    ENOENT);
	host_unmount(&mountp);

	/* Validate the persisted bytes through a fresh directory traversal too. */
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	CHECK_ERROR(lookup_child(mountp.m_root, "Rollback End Marker.txt",
	    &created), ENOENT);
	CHECK_ERROR(lookup_child(mountp.m_root, "STALE.TXT", &created),
	    ENOENT);
	host_unmount(&mountp);
	destroy_image(&image);

	/* Repeat with a second fault in the first rollback flush.  Restoration
	 * continues while the mount is still writable; a later exact-slot flush
	 * persists the complete sector, but any rollback error still freezes it.
	 */
	current_stage = "double-fault create restores FAT end marker";
	current_fault_ordinal = 2U;
	created = NULL;
	format_image(&image, ZEDBSD_FAT32, 1U, 2U);
	root = image_sector(&image, cluster_lba(&image, image.root_cluster));
	set_dirent(root + 7U * 32U, stale_sfn, 0x20U, 0U, 0U,
	    ZEDBSD_FAT32);
	memcpy(saved, root + 4U * 32U, sizeof(saved));
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	schedule_two_write_failures(&image, 2U, 3U);
	CHECK_ERROR(create_child(mountp.m_root, "Rollback End Marker.txt",
	    &created), EIO);
	CHECK(created == NULL);
	CHECK(image.fail_write_attempt == 0U);
	CHECK(image.fail_write_attempt2 == 0U);
	CHECK(memcmp(saved, root + 4U * 32U, sizeof(saved)) == 0);
	CHECK_ERROR(lookup_child(mountp.m_root, "STALE.TXT", &created),
	    ENOENT);
	CHECK_ERROR(create_child(mountp.m_root, "FROZEN.TXT", &created),
	    EROFS);
	CHECK(created == NULL);
	host_unmount(&mountp);
	destroy_image(&image);
}

static void
check_mount_faults_and_validation(void)
{
	struct memory_image image;
	struct mount mountp;
	enum bootfat_type type;
	uint8_t saved;

	current_type = ZEDBSD_FAT12;
	format_image(&image, ZEDBSD_FAT12, 1U, 2U);
	image.disk.d_block_size = 1024U;
	CHECK_ERROR(fat_probe_type(&image.disk, &type), EOPNOTSUPP);
	image.disk.d_block_size = SECTOR_SIZE;
	image.fail_reads = 1U;
	CHECK_ERROR(fat_filesystem_type.probe(&image.disk), EIO);
	CHECK_ERROR(fat_filesystem_type.probe(&image.disk), 0);

	image.fail_reads = 1U;
	CHECK_ERROR(host_mount(&image, 0U, &mountp), EIO);
	CHECK(mountp.m_data == NULL);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	host_unmount(&mountp);

	saved = image.bytes[13];
	image.bytes[13] = 0;
	CHECK_ERROR(fat_filesystem_type.probe(&image.disk), EIO);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), EIO);
	CHECK(mountp.m_data == NULL);
	image.bytes[13] = saved;
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	host_unmount(&mountp);
	destroy_image(&image);
}

static void
check_large_logical_sector(void)
{
	struct memory_image image;
	struct mount mountp;
	uint64_t identity;

	current_type = ZEDBSD_FAT12;
	format_image(&image, ZEDBSD_FAT12, 2U, 2U);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	check_probe_and_initial_contents(&image, &mountp, &identity);
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	host_unmount(&mountp);
	check_fat_copies(&image);
	destroy_image(&image);
}

static void
run_variant(enum bootfat_type type)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *persisted;
	uint64_t hello_identity;
	uint64_t remount_identity;
	struct disk *identity_disk;
	const char *persisted_name = variant_name(type, "PERSIST.TXT",
	    "Persisted Long Name.txt");
	unsigned writes_before;

	current_type = type;
	format_image(&image, type, 1U, 2U);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	current_stage = "initial contents";
	check_probe_and_initial_contents(&image, &mountp, &hello_identity);
	host_check_idle_mount(&mountp);
	current_stage = "file mutations";
	check_file_mutations(&image, &mountp);
	host_check_idle_mount(&mountp);
	current_stage = "namespace and orphan lifetime";
	check_namespace_and_orphans(&image, &mountp);
	host_check_idle_mount(&mountp);
	check_rename_descendant_path_preflight(&image, &mountp);
	host_check_idle_mount(&mountp);
	check_open_writer_path_truncate(&image, &mountp);
	host_check_idle_mount(&mountp);
	check_orphan_fd_mutation(&image, &mountp);
	host_check_idle_mount(&mountp);
	check_orphan_reclaim_retry(&image, &mountp);
	host_check_idle_mount(&mountp);
	check_mount_sync_open_writer(&image, &mountp);
	host_check_idle_mount(&mountp);
	check_close_metadata_retry(&image, &mountp);
	host_check_idle_mount(&mountp);
	current_stage = "I/O fault retry";
	check_io_fault_retry(&mountp);
	host_check_idle_mount(&mountp);
	current_stage = "sync and remount";
	persisted = create_payload(mountp.m_root, persisted_name,
	    "persisted", 9U);
	inode_release(persisted);
	current_stage = "persisted close boundary";
	host_check_idle_mount(&mountp);
	writes_before = image.writes;
	CHECK_ERROR(mountp.m_type->sync(&mountp), 0);
	CHECK(image.syncs != 0U);
	CHECK(image.writes >= writes_before);
	current_stage = "post-sync idle boundary";
	host_check_idle_mount(&mountp);
	current_stage = "sync and remount";
	host_unmount(&mountp);
	check_fat_copies(&image);

	image.fail_reads = 1U;
	CHECK_ERROR(host_mount(&image, 0U, &mountp), EIO);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	read_path(&mountp, persisted_name, "persisted", 9U);
	CHECK_ERROR(lookup_child(mountp.m_root, "HELLO.TXT", &persisted), 0);
	CHECK_ERROR(fat_file_backing_identity(persisted, &identity_disk,
	    &remount_identity), 0);
	CHECK(identity_disk == &image.disk);
	CHECK(remount_identity == hello_identity);
	inode_release(persisted);
	current_stage = "remount idle boundary";
	host_check_idle_mount(&mountp);
	current_stage = "sync and remount";
	host_unmount(&mountp);

	check_read_only(&image, 0);
	check_read_only(&image, 1);
	destroy_image(&image);
	check_no_space(type);
}

int
main(void)
{
	check_mount_faults_and_validation();
	run_variant(ZEDBSD_FAT12);
	run_variant(ZEDBSD_FAT16);
	run_variant(ZEDBSD_FAT32);
	check_orphan_setattr_slot_reuse();
	check_replacement_payload_write_failure();
	check_fat12_16_insert_slot_failures();
	check_large_logical_sector();
	check_maximum_lfn_name();
	check_lfn_namespace_faults();
	check_dotdot_rename_faults();
	check_sector_boundary_lfn_faults();
	check_partial_shrink_faults();
	check_partial_grow_faults();
	check_mirrored_fat_rollback_failure_freezes_mount();
	check_failed_create_restores_end_marker();
	current_type = 0;
	current_stage = "final fixture cleanup";
	current_fault_ordinal = 0;
	CHECK(inode_allocations == inode_destructions);
	CHECK(inode_free_attempts == inode_destructions);
	for (unsigned i = 0; i < HOST_INODE_MAX; i++)
		CHECK(host_inode_cache[i] == NULL);
	CHECK(namecache_removes != 0U);
	CHECK(namecache_purges != 0U);
	printf("KA-T100/KA-T101: PASS (%u checks; native FAT12/16/32 VFS, "
	    "two FAT copies, 1024-byte logical sectors)\n", checks);
	return 0;
}
