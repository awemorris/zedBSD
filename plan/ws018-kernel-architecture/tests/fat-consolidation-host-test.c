/*
 * WS018 KA-T090: consolidated FAT12/FAT16/FAT32 host fixture
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <kern/fat.h>
#include <kern/fs.h>
#include <kern/namecache.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE 512U
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

struct memory_image {
	uint8_t *bytes;
	uint32_t sectors;
	uint32_t reserved;
	uint32_t fat_sectors;
	uint32_t root_sectors;
	uint32_t root_start;
	uint32_t data_start;
	uint32_t cluster_count;
	uint32_t root_cluster;
	enum bootfat_type type;
	unsigned reads;
	unsigned writes;
};

static unsigned checks;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr, "KA-T090: failed at %s:%d: %s\n",    \
			    __FILE__, __LINE__, #expression);                    \
			exit(1);                                             \
		}                                                            \
	} while (0)

/*
 * FAT_MUTATION intentionally groups the compatibility and native mutation
 * routines in one linker section.  Referencing the former consequently keeps
 * some unused native routines alive in this host binary.  These inert shims
 * satisfy only that link boundary; KA-T090 never invokes the native VFS path.
 */
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

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	(void)mutex;
	(void)rank;
	(void)name;
	return 0;
}

void
namecache_remove(struct inode *parent, const struct componentname *name)
{
	(void)parent;
	(void)name;
}

void
namecache_purge_inode(struct inode *inode)
{
	(void)inode;
}

struct inode *
inode_alloc(struct mount *mountp)
{
	(void)mountp;
	return NULL;
}

void
inode_free(struct inode *inode)
{
	(void)inode;
}

int
inode_get(struct mount *mountp, ino_t ino, struct inode **result)
{
	(void)mountp;
	(void)ino;
	(void)result;
	return ENOENT;
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
disk_sync(struct disk *disk)
{
	(void)disk;
	return 0;
}

int
disk_read(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	(void)disk;
	(void)block;
	(void)count;
	(void)data;
	return EIO;
}

int
disk_read_direct(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	return disk_read(disk, block, count, data);
}

int
disk_write_filesystem(struct disk *disk, uint64_t block, uint32_t count,
	const void *data)
{
	(void)disk;
	(void)block;
	(void)count;
	(void)data;
	return EIO;
}

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
	return image->data_start + cluster - 2U;
}

static uint32_t
fat_end_of_chain(enum bootfat_type type)
{
	return type == ZEDBSD_FAT12 ? 0x0fffU :
	       type == ZEDBSD_FAT16 ? 0xffffU : 0x0fffffffU;
}

static void
set_fat_entry(struct memory_image *image, uint32_t cluster, uint32_t value)
{
	uint8_t *fat = image_sector(image, image->reserved);
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

static void
put_cluster_bytes(struct memory_image *image, uint32_t cluster,
	const uint8_t *input, size_t length)
{
	uint8_t *output = image_sector(image, cluster_lba(image, cluster));

	CHECK(length <= SECTOR_SIZE);
	memcpy(output, input, length);
}

static void
format_image(struct memory_image *image, enum bootfat_type type)
{
	static const uint8_t hello_sfn[11] = {
	    'H', 'E', 'L', 'L', 'O', ' ', ' ', ' ', 'T', 'X', 'T'
	};
	static const char long_name[] = "Long File Name.txt";
	uint8_t hello[700];
	uint8_t *boot;
	uint8_t *root;
	uint32_t eoc = fat_end_of_chain(type);
	unsigned i;

	memset(image, 0, sizeof(*image));
	image->type = type;
	if (type == ZEDBSD_FAT12) {
		image->sectors = 128U;
		image->reserved = 1U;
		image->fat_sectors = 1U;
		image->root_sectors = 4U;
	} else if (type == ZEDBSD_FAT16) {
		image->sectors = 5000U;
		image->reserved = 1U;
		image->fat_sectors = 20U;
		image->root_sectors = 4U;
	} else {
		image->sectors = 67000U;
		image->reserved = 32U;
		image->fat_sectors = 524U;
		image->root_cluster = 2U;
	}
	image->root_start = image->reserved + image->fat_sectors;
	image->data_start = image->root_start + image->root_sectors;
	image->cluster_count = image->sectors - image->data_start;
	image->bytes = calloc((size_t)image->sectors, SECTOR_SIZE);
	CHECK(image->bytes != NULL);

	boot = image_sector(image, 0);
	boot[0] = 0xebU;
	boot[1] = 0x3cU;
	boot[2] = 0x90U;
	memcpy(boot + 3, "ZEDBSD  ", 8);
	put16(boot + 11, SECTOR_SIZE);
	boot[13] = 1U;
	put16(boot + 14, (uint16_t)image->reserved);
	boot[16] = 1U;
	put16(boot + 17, type == ZEDBSD_FAT32 ? 0U : 64U);
	put16(boot + 19, image->sectors <= UINT16_MAX ?
	    (uint16_t)image->sectors : 0U);
	boot[21] = 0xf8U;
	put16(boot + 22, type == ZEDBSD_FAT32 ? 0U :
	    (uint16_t)image->fat_sectors);
	put16(boot + 24, 63U);
	put16(boot + 26, 255U);
	if (image->sectors > UINT16_MAX)
		put32(boot + 32, image->sectors);
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
	set_fat_entry(image, 3U, 4U);
	set_fat_entry(image, 4U, eoc);

	root = type == ZEDBSD_FAT32 ?
	    image_sector(image, cluster_lba(image, image->root_cluster)) :
	    image_sector(image, image->root_start);
	set_dirent(root, hello_sfn, 0x20U, 3U, sizeof(hello), type);
	for (i = 0; i < ARRAY_COUNT(hello); i++)
		hello[i] = (uint8_t)('A' + i % 26U);
	put_cluster_bytes(image, 3U, hello, SECTOR_SIZE);
	put_cluster_bytes(image, 4U, hello + SECTOR_SIZE,
	    sizeof(hello) - SECTOR_SIZE);

	if (type == ZEDBSD_FAT32) {
		uint16_t units[FAT_LFN_MAX_UNITS];
		uint8_t sfn[11];
		unsigned unit_count;
		unsigned lfn_count;
		static const uint8_t payload[] = "long payload";

		CHECK(fat_utf8_to_utf16(long_name, units, &unit_count));
		CHECK(fat_sfn_make_alias(long_name, 1U, sfn));
		lfn_count = (unit_count + 12U) / 13U;
		for (i = 0; i < lfn_count; i++)
			fat_lfn_build_entry(root + (i + 1U) * 32U, units,
			    unit_count, lfn_count - i, fat_lfn_checksum(sfn));
		set_dirent(root + (lfn_count + 1U) * 32U, sfn, 0x20U, 5U,
		    sizeof(payload) - 1U, type);
		set_fat_entry(image, 5U, eoc);
		put_cluster_bytes(image, 5U, payload, sizeof(payload) - 1U);
	}
}

static int
memory_read(const void *context, uint32_t lba, void *buffer)
{
	struct memory_image *image = (struct memory_image *)context;

	if (image == NULL || buffer == NULL || lba >= image->sectors)
		return 0;
	memcpy(buffer, image->bytes + (size_t)lba * SECTOR_SIZE,
	    SECTOR_SIZE);
	image->reads++;
	return 1;
}

static int
memory_write(void *context, uint32_t lba, const void *buffer)
{
	struct memory_image *image = context;

	if (image == NULL || buffer == NULL || lba >= image->sectors)
		return 0;
	memcpy(image->bytes + (size_t)lba * SECTOR_SIZE, buffer, SECTOR_SIZE);
	image->writes++;
	return 1;
}

static struct boot_volume
memory_volume(struct memory_image *image, int writable)
{
	struct boot_volume volume;

	memset(&volume, 0, sizeof(volume));
	volume.context = image;
	volume.sector_size = SECTOR_SIZE;
	volume.read = memory_read;
	volume.write = writable ? memory_write : NULL;
	return volume;
}

static const struct bootfs_driver *
driver_for(enum bootfat_type type)
{
	return type == ZEDBSD_FAT12 ? &bootfat12_driver :
	       type == ZEDBSD_FAT16 ? &bootfat16_driver : &bootfat32_driver;
}

static void
mount_image(struct memory_image *image, int writable, struct bootfs *filesystem)
{
	static const struct bootfs_driver *const drivers[] = {
	    &bootfat12_driver, &bootfat16_driver, &bootfat32_driver,
	};
	struct boot_volume volume = memory_volume(image, writable);

	CHECK(bootfat12_driver.probe(&volume) ==
	    (image->type == ZEDBSD_FAT12 ? ZEDBSD_FS_OK :
	    ZEDBSD_FS_UNSUPPORTED));
	CHECK(bootfat16_driver.probe(&volume) ==
	    (image->type == ZEDBSD_FAT16 ? ZEDBSD_FS_OK :
	    ZEDBSD_FS_UNSUPPORTED));
	CHECK(bootfat32_driver.probe(&volume) ==
	    (image->type == ZEDBSD_FAT32 ? ZEDBSD_FS_OK :
	    ZEDBSD_FS_UNSUPPORTED));
	CHECK(bootfs_mount_result(filesystem, &volume, drivers,
	    ARRAY_COUNT(drivers)) == ZEDBSD_FS_OK);
	CHECK(filesystem->driver == driver_for(image->type));
	CHECK(bootfat_state(filesystem)->type == image->type);
}

static void
check_initial_contents(struct memory_image *image, struct bootfs *filesystem)
{
	static const char long_name[] = "Long File Name.txt";
	struct bootfs_dirent entry;
	struct bootfs_file file;
	uint8_t hello[700];
	uint8_t expected[700];
	unsigned i;

	CHECK(bootfs_open_result(filesystem, "HELLO.TXT", &file) ==
	    ZEDBSD_FS_OK);
	CHECK(file.size == sizeof(hello));
	CHECK(bootfs_file_read_result(&file, 0, hello, sizeof(hello), NULL,
	    NULL) == ZEDBSD_FS_OK);
	for (i = 0; i < ARRAY_COUNT(expected); i++)
		expected[i] = (uint8_t)('A' + i % 26U);
	CHECK(memcmp(hello, expected, sizeof(hello)) == 0);
	CHECK(bootfs_readdir_result(filesystem, "", 0, &entry) ==
	    ZEDBSD_FS_OK);
	CHECK(strcmp(entry.name, image->type == ZEDBSD_FAT32 ?
	    "HELLO.TXT" : "hello.txt") == 0);
	CHECK(entry.size == sizeof(hello));
	CHECK(bootfs_stat_result(filesystem, "HELLO.TXT", &entry) ==
	    ZEDBSD_FS_OK);

	if (image->type == ZEDBSD_FAT32) {
		uint32_t cluster;
		uint32_t lba;
		uint16_t offset;
		uint8_t attributes;
		char payload[12];

		CHECK(bootfs_open_result(filesystem, long_name, &file) ==
		    ZEDBSD_FS_OK);
		CHECK(file.size == sizeof(payload));
		CHECK(bootfs_file_read_result(&file, 0, payload,
		    sizeof(payload), NULL, NULL) == ZEDBSD_FS_OK);
		CHECK(memcmp(payload, "long payload", sizeof(payload)) == 0);
		CHECK(bootfs_readdir_result(filesystem, "", 1, &entry) ==
		    ZEDBSD_FS_OK);
		CHECK(strcmp(entry.name, long_name) == 0);
		CHECK(bootfat_stat_location_casefold(filesystem,
		    "long file name.TXT", &entry, &lba, &offset, &cluster,
		    &attributes) == ZEDBSD_FS_OK);
		CHECK(strcmp(entry.name, long_name) == 0);
	}
}

static void
check_readback(struct bootfs *filesystem, const char *path,
	const void *expected, uint32_t length)
{
	struct bootfs_file file;
	uint8_t buffer[700];

	CHECK(length <= sizeof(buffer));
	CHECK(bootfs_open_result(filesystem, path, &file) == ZEDBSD_FS_OK);
	CHECK(file.size == length);
	if (length != 0U) {
		CHECK(bootfs_file_read_result(&file, 0, buffer, length, NULL,
		    NULL) == ZEDBSD_FS_OK);
		CHECK(memcmp(buffer, expected, length) == 0);
	}
}

static const char *
name_for(enum bootfat_type type, const char *sfn, const char *lfn)
{
	return type == ZEDBSD_FAT32 ? lfn : sfn;
}

static void
check_file_mutations(struct memory_image *image, struct bootfs *filesystem)
{
	const char *path = name_for(image->type, "RW.TXT",
	    "Created Long Name.txt");
	struct bootfs_file file;
	uint8_t sparse_expected[9] = {
	    'a', 'Z', 'c', 'D', 'E', 'F', 0, 0, 'Q'
	};
	uint8_t extended[600];
	uint32_t free_before;
	uint32_t free_after;
	unsigned writes_before = image->writes;

	CHECK(bootfat_count_free_clusters(filesystem, &free_before) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_create_result(filesystem, path, &file) == ZEDBSD_FS_OK);
	CHECK(file.size == 0U);
	CHECK(bootfs_file_write_result(&file, 0, "abc", 3) == ZEDBSD_FS_OK);
	CHECK(bootfs_file_write_result(&file, file.size, "DEF", 3) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_file_write_result(&file, 1, "Z", 1) == ZEDBSD_FS_OK);
	CHECK(bootfs_file_write_result(&file, 8, "Q", 1) == ZEDBSD_FS_OK);
	CHECK(file.size == sizeof(sparse_expected));
	CHECK(bootfs_file_flush_result(&file) == ZEDBSD_FS_OK);
	CHECK(image->writes > writes_before);
	check_readback(filesystem, path, sparse_expected,
	    sizeof(sparse_expected));

	CHECK(bootfs_file_truncate_result(&file, sizeof(extended)) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_file_flush_result(&file) == ZEDBSD_FS_OK);
	memset(extended, 0, sizeof(extended));
	memcpy(extended, sparse_expected, sizeof(sparse_expected));
	check_readback(filesystem, path, extended, sizeof(extended));
	CHECK(bootfs_file_truncate_result(&file, 2) == ZEDBSD_FS_OK);
	CHECK(bootfs_file_flush_result(&file) == ZEDBSD_FS_OK);
	check_readback(filesystem, path, "aZ", 2);

	/* create of an existing regular file has truncate-to-zero semantics. */
	CHECK(bootfs_create_result(filesystem, path, &file) == ZEDBSD_FS_OK);
	CHECK(file.size == 0U);
	CHECK(bootfs_file_flush_result(&file) == ZEDBSD_FS_OK);
	check_readback(filesystem, path, "", 0);
	CHECK(bootfat_count_free_clusters(filesystem, &free_after) ==
	    ZEDBSD_FS_OK);
	CHECK(free_after == free_before);
}

static void
create_payload(struct bootfs *filesystem, const char *path,
	const char *payload)
{
	struct bootfs_file file;
	uint32_t length = (uint32_t)strlen(payload);

	CHECK(bootfs_create_result(filesystem, path, &file) == ZEDBSD_FS_OK);
	CHECK(bootfs_file_write_result(&file, 0, payload, length) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_file_flush_result(&file) == ZEDBSD_FS_OK);
}

static void
check_namespace_mutations(struct memory_image *image,
	struct bootfs *filesystem)
{
	const char *child = name_for(image->type, "A/CHILD.TXT",
	    "A/Child Long Name.txt");
	const char *renamed = name_for(image->type, "A/RENAMED.TXT",
	    "A/Renamed Long Name.txt");
	const char *moved = name_for(image->type, "B/MOVED.TXT",
	    "B/Moved Long Name.txt");
	const char *target = name_for(image->type, "B/TARGET.TXT",
	    "B/Target Long Name.txt");
	struct bootfs_file file;

	CHECK(bootfs_mkdir_result(filesystem, "A") == ZEDBSD_FS_OK);
	CHECK(bootfs_mkdir_result(filesystem, "B") == ZEDBSD_FS_OK);
	create_payload(filesystem, child, "source");
	CHECK(bootfs_rmdir_result(filesystem, "A") ==
	    ZEDBSD_FS_NOT_EMPTY);
	CHECK(bootfs_rename_result(filesystem, child, renamed) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_open_result(filesystem, child, &file) ==
	    ZEDBSD_FS_NOT_FOUND);
	CHECK(bootfs_rename_result(filesystem, renamed, moved) ==
	    ZEDBSD_FS_OK);
	check_readback(filesystem, moved, "source", 6);

	create_payload(filesystem, target, "target");
	CHECK(bootfs_rename_result(filesystem, moved, target) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_open_result(filesystem, moved, &file) ==
	    ZEDBSD_FS_NOT_FOUND);
	check_readback(filesystem, target, "source", 6);
	CHECK(bootfs_unlink_result(filesystem, target) == ZEDBSD_FS_OK);
	CHECK(bootfs_open_result(filesystem, target, &file) ==
	    ZEDBSD_FS_NOT_FOUND);

	CHECK(bootfs_mkdir_result(filesystem, "A/SUB") == ZEDBSD_FS_OK);
	CHECK(bootfs_rename_result(filesystem, "A/SUB", "B/SUB") ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_rmdir_result(filesystem, "B/SUB") == ZEDBSD_FS_OK);
	CHECK(bootfs_unlink_result(filesystem, "A") ==
	    ZEDBSD_FS_IS_DIRECTORY);
	CHECK(bootfs_rmdir_result(filesystem, "A") == ZEDBSD_FS_OK);
	CHECK(bootfs_rmdir_result(filesystem, "B") == ZEDBSD_FS_OK);
	CHECK(bootfs_mkdir_result(filesystem, "EMPTY") == ZEDBSD_FS_OK);
	CHECK(bootfs_rmdir_result(filesystem, "EMPTY") == ZEDBSD_FS_OK);
}

static void
check_read_only(struct memory_image *image)
{
	struct bootfs filesystem;
	struct bootfs_file file;
	char byte;

	mount_image(image, 0, &filesystem);
	CHECK(bootfs_open_result(&filesystem, "HELLO.TXT", &file) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_file_read_result(&file, 0, &byte, 1, NULL, NULL) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_create_result(&filesystem, "RO.TXT", &file) ==
	    ZEDBSD_FS_READ_ONLY);
	CHECK(bootfs_mkdir_result(&filesystem, "RODIR") ==
	    ZEDBSD_FS_READ_ONLY);
	CHECK(bootfs_unlink_result(&filesystem, "HELLO.TXT") ==
	    ZEDBSD_FS_READ_ONLY);
	CHECK(bootfs_rename_result(&filesystem, "HELLO.TXT", "OTHER.TXT") ==
	    ZEDBSD_FS_READ_ONLY);
	CHECK(bootfs_open_result(&filesystem, "HELLO.TXT", &file) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_file_write_result(&file, 0, "x", 1) ==
	    ZEDBSD_FS_READ_ONLY);
	CHECK(bootfs_file_truncate_result(&file, 0) == ZEDBSD_FS_READ_ONLY);
	CHECK(bootfs_file_flush_result(&file) == ZEDBSD_FS_READ_ONLY);
}

static void
mark_all_clusters_allocated(struct memory_image *image)
{
	uint32_t cluster;
	uint32_t eoc = fat_end_of_chain(image->type);

	for (cluster = 2U; cluster < image->cluster_count + 2U; cluster++)
		set_fat_entry(image, cluster, eoc);
}

static void
check_no_space(struct memory_image *image)
{
	struct bootfs filesystem;
	struct bootfs_file file;
	uint32_t free_clusters;

	mount_image(image, 1, &filesystem);
	CHECK(bootfs_create_result(&filesystem, "FULL.TXT", &file) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_file_flush_result(&file) == ZEDBSD_FS_OK);
	mark_all_clusters_allocated(image);
	mount_image(image, 1, &filesystem);
	CHECK(bootfat_count_free_clusters(&filesystem, &free_clusters) ==
	    ZEDBSD_FS_OK);
	CHECK(free_clusters == 0U);
	CHECK(bootfs_open_result(&filesystem, "FULL.TXT", &file) ==
	    ZEDBSD_FS_OK);
	CHECK(bootfs_file_write_result(&file, 0, "x", 1) ==
	    ZEDBSD_FS_NO_SPACE);
	CHECK(file.size == 0U);
}

static void
run_variant(enum bootfat_type type)
{
	struct memory_image image;
	struct bootfs filesystem;
	struct bootfs remounted;
	const char *persisted = name_for(type, "PERSIST.TXT",
	    "Persisted Long Name.txt");

	format_image(&image, type);
	mount_image(&image, 1, &filesystem);
	check_initial_contents(&image, &filesystem);
	check_file_mutations(&image, &filesystem);
	check_namespace_mutations(&image, &filesystem);
	create_payload(&filesystem, persisted, "persisted");

	/* A fresh mount proves that explicit file flushes reached the image. */
	mount_image(&image, 1, &remounted);
	check_initial_contents(&image, &remounted);
	check_readback(&remounted, persisted, "persisted", 9);
	check_read_only(&image);
	check_no_space(&image);
	free(image.bytes);
}

int
main(void)
{
	run_variant(ZEDBSD_FAT12);
	run_variant(ZEDBSD_FAT16);
	run_variant(ZEDBSD_FAT32);
	printf("KA-T090: PASS (%u checks; FAT12/16/32 legacy bootfs)\n",
	    checks);
	return 0;
}
