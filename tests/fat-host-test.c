/* Host-side regression tests for the Boots FAT12/FAT16 driver. */

#include "core/fat16.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_BASE_LBA 100U

struct test_disk {
	uint8_t bpb[512];
	/* Two backed FAT sectors so 12-bit entries can straddle them. */
	uint8_t fat[1024];
	uint8_t root[512];
	uint8_t data[8][512];
	uint32_t fat_lba;
	uint32_t root_lba;
	uint32_t data_lba;
	unsigned write_count;
	int fail_reads;
	int fail_writes;
};

static void put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
}

static void put32(uint8_t *bytes, uint32_t value)
{
	put16(bytes, value);
	put16(bytes + 2, value >> 16);
}

static int test_read(const void *context, uint32_t absolute_lba, void *buffer)
{
	const struct test_disk *disk = context;
	uint32_t lba;

	if (disk->fail_reads || absolute_lba < TEST_BASE_LBA)
		return 0;
	lba = absolute_lba - TEST_BASE_LBA;
	memset(buffer, 0, 512);
	if (!lba)
		memcpy(buffer, disk->bpb, 512);
	else if (lba == disk->fat_lba)
		memcpy(buffer, disk->fat, 512);
	else if (lba == disk->fat_lba + 1)
		memcpy(buffer, disk->fat + 512, 512);
	else if (lba == disk->root_lba)
		memcpy(buffer, disk->root, 512);
	else if (lba >= disk->data_lba && lba < disk->data_lba + 8)
		memcpy(buffer, disk->data[lba - disk->data_lba], 512);
	return 1;
}

static int test_write(void *context, uint32_t absolute_lba,
		      const void *buffer)
{
	struct test_disk *disk = context;
	uint32_t lba;
	uint8_t *destination;

	if (disk->fail_writes || absolute_lba < TEST_BASE_LBA)
		return 0;
	lba = absolute_lba - TEST_BASE_LBA;
	if (!lba)
		destination = disk->bpb;
	else if (lba == disk->fat_lba)
		destination = disk->fat;
	else if (lba == disk->fat_lba + 1)
		destination = disk->fat + 512;
	else if (lba == disk->root_lba)
		destination = disk->root;
	else if (lba >= disk->data_lba && lba < disk->data_lba + 8)
		destination = disk->data[lba - disk->data_lba];
	else
		return 0;
	memcpy(destination, buffer, 512);
	disk->write_count++;
	return 1;
}

static void make_disk(struct test_disk *disk, uint16_t logical_sector_size)
{
	uint8_t scale = logical_sector_size / 512;
	uint16_t fat_logical_sectors = 17;

	memset(disk, 0, sizeof(*disk));
	put16(disk->bpb + 11, logical_sector_size);
	disk->bpb[13] = 1;
	put16(disk->bpb + 14, 1);
	disk->bpb[16] = 1;
	put16(disk->bpb + 17, 16);
	put16(disk->bpb + 19, 4104);
	put16(disk->bpb + 22, fat_logical_sectors);
	disk->fat_lba = scale;
	disk->root_lba = scale + fat_logical_sectors * scale;
	disk->data_lba = disk->root_lba + 1;
	put16(disk->fat + 4, 0xffff);
	put16(disk->fat + 6, 0xffff);
	put16(disk->fat + 8, 0xffff);
	memcpy(disk->root, "KERNEL  BIN", 11);
	disk->root[11] = 0x20;
	put16(disk->root + 26, 2);
	put32(disk->root + 28, 5);
	memcpy(disk->data[0], "hello", 5);
	memcpy(disk->root + 32, "CMD        ", 11);
	disk->root[32 + 11] = 0x10;
	put16(disk->root + 32 + 26, 3);
	memcpy(disk->data[scale], ".          ", 11);
	disk->data[scale][11] = 0x10;
	put16(disk->data[scale] + 26, 3);
	memcpy(disk->data[scale] + 32, "..         ", 11);
	disk->data[scale][32 + 11] = 0x10;
	memcpy(disk->data[scale] + 64, "REMACS  NAP", 11);
	disk->data[scale][64 + 11] = 0x20;
	put16(disk->data[scale] + 64 + 26, 4);
	put32(disk->data[scale] + 64 + 28, 5);
	memcpy(disk->data[2U * scale], "remac", 5);
}

static void test_fat16(uint16_t logical_sector_size)
{
	const struct boots_filesystem_driver *const drivers[] = {
		&boots_fat16_driver,
	};
	struct test_disk disk;
	struct boots_volume volume;
	struct boots_filesystem filesystem;
	struct boots_file file;
	struct boots_dirent entry;
	uint32_t lba = 0;
	char buffer[6] = { 0 };
	uint8_t scale = logical_sector_size / 512;

	make_disk(&disk, logical_sector_size);
	volume.context = &disk;
	volume.start_lba = TEST_BASE_LBA;
	volume.sector_size = 512;
	volume.read = test_read;
	/* Keep this regression focused on the legacy read-only contract.  The
	 * writable FAT16 path has its own destructive host-side test image. */
	volume.write = 0;
	assert(boots_fs_mount(&filesystem, &volume, drivers, 1));
	assert(!strcmp(filesystem.driver->name, "fat16"));
	assert(boots_fs_open_result(&filesystem, "/missing.bin", &file) ==
	       BOOTS_FS_NOT_FOUND);
	assert(boots_fs_open_result(&filesystem, "/bad/path", &file) ==
	       BOOTS_FS_NOT_FOUND);
	assert(boots_fs_create_result(&filesystem, "/new.bin", &file) ==
	       BOOTS_FS_READ_ONLY);
	assert(boots_fs_open(&filesystem, "/kernel.bin", &file));
	assert(file.size == 5);
	assert(boots_file_read(&file, 0, buffer, 5));
	assert(!strcmp(buffer, "hello"));
	memset(buffer, 0, sizeof(buffer));
	assert(boots_fs_open(&filesystem, "/cmd/remacs.nap", &file));
	assert(file.size == 5);
	assert(boots_file_read(&file, 0, buffer, 5));
	assert(!strcmp(buffer, "remac"));
	assert(boots_fs_readdir(&filesystem, "/", 0, &entry));
	assert(!strcmp(entry.name, "KERNEL.BIN"));
	assert(entry.size == 5 && entry.attributes == 0x20);
	assert(boots_fs_readdir(&filesystem, "/cmd", 0, &entry));
	assert(!strcmp(entry.name, "REMACS.NAP"));
	assert(!boots_fs_readdir(&filesystem, "/subdir", 0, &entry));
	assert(boots_fs_readdir_result(&filesystem, "/subdir", 0, &entry) ==
	       BOOTS_FS_NOT_FOUND);
	assert(boots_file_write_result(&file, 0, "x", 1) ==
	       BOOTS_FS_READ_ONLY);
	assert(boots_file_truncate_result(&file, 0) == BOOTS_FS_READ_ONLY);
	assert(boots_file_flush_result(&file) == BOOTS_FS_READ_ONLY);
	assert(boots_fs_stat_result(&filesystem, "/cmd/remacs.nap", &entry) ==
	       BOOTS_FS_OK);
	assert(!strcmp(entry.name, "REMACS.NAP") && entry.size == 5);
	assert(boots_file_contiguous_lba(&file, &lba));
	assert(lba == TEST_BASE_LBA + disk.data_lba + 2U * scale);
}

static void test_volume_write_contract(void)
{
	struct test_disk disk;
	struct boots_volume volume;
	uint8_t original[512], replacement[512], observed[512];

	make_disk(&disk, 512);
	memcpy(original, disk.data[0], sizeof(original));
	memset(replacement, 0xa5, sizeof(replacement));
	volume.context = &disk;
	volume.start_lba = TEST_BASE_LBA;
	volume.sector_size = 512;
	volume.read = test_read;
	volume.write = test_write;
	assert(boots_volume_write_result(&volume, disk.data_lba,
					  replacement) == BOOTS_FS_OK);
	assert(disk.write_count == 1);
	assert(boots_volume_read_result(&volume, disk.data_lba, observed) ==
	       BOOTS_FS_OK);
	assert(!memcmp(observed, replacement, sizeof(observed)));
	assert(boots_volume_write(&volume, disk.data_lba, original));
	assert(!memcmp(disk.data[0], original, sizeof(original)));

	volume.write = 0;
	assert(boots_volume_write_result(&volume, disk.data_lba,
					  replacement) == BOOTS_FS_READ_ONLY);
	volume.write = test_write;
	disk.fail_writes = 1;
	assert(boots_volume_write_result(&volume, disk.data_lba,
					  replacement) == BOOTS_FS_IO_ERROR);
	disk.fail_writes = 0;
	volume.start_lba = UINT32_MAX;
	assert(boots_volume_write_result(&volume, 1, replacement) ==
	       BOOTS_FS_INVALID_ARGUMENT);
	volume.start_lba = TEST_BASE_LBA;
	volume.sector_size = 1024;
	assert(boots_volume_write_result(&volume, disk.data_lba,
					  replacement) ==
	       BOOTS_FS_INVALID_ARGUMENT);
}

static void fat12_set(uint8_t *fat, uint32_t cluster, uint16_t value)
{
	uint32_t offset = cluster + cluster / 2;

	if (cluster & 1) {
		fat[offset] = (fat[offset] & 0x0f) | ((value << 4) & 0xf0);
		fat[offset + 1] = value >> 4;
	} else {
		fat[offset] = value & 0xff;
		fat[offset + 1] = (fat[offset + 1] & 0xf0) |
			((value >> 8) & 0x0f);
	}
}

static uint16_t fat12_get(const uint8_t *fat, uint32_t cluster)
{
	uint32_t offset = cluster + cluster / 2;
	uint16_t value = fat[offset] | ((uint16_t)fat[offset + 1] << 8);

	return (cluster & 1) ? value >> 4 : value & 0xfff;
}

static void make_fat12_disk(struct test_disk *disk)
{
	make_disk(disk, 512);
	/* 1024 total sectors leaves ~1005 clusters: a FAT12 volume. */
	put16(disk->bpb + 19, 1024);
	memset(disk->fat, 0, sizeof(disk->fat));
	fat12_set(disk->fat, 0, 0xff8);
	fat12_set(disk->fat, 1, 0xfff);
	fat12_set(disk->fat, 2, 0xfff); /* KERNEL.BIN */
	fat12_set(disk->fat, 3, 0xfff); /* CMD directory */
	fat12_set(disk->fat, 4, 0xfff); /* CMD/REMACS.NAP */
	/* Entry 341 starts at FAT byte offset 511 and straddles the
	 * sector boundary. */
	fat12_set(disk->fat, 341, 342);
	fat12_set(disk->fat, 342, 0xfff);
	memcpy(disk->root + 64, "LONG    BIN", 11);
	disk->root[64 + 11] = 0x20;
	put16(disk->root + 64 + 26, 341);
	put32(disk->root + 64 + 28, 600);
}

static void test_fat12(void)
{
	const struct boots_filesystem_driver *const drivers[] = {
		&boots_fat12_driver,
	};
	struct test_disk disk;
	struct boots_volume volume;
	struct boots_filesystem filesystem;
	struct boots_file file;
	struct boots_dirent entry;
	uint32_t lba = 0;
	char buffer[6] = { 0 };
	uint8_t long_buffer[600];

	make_fat12_disk(&disk);
	volume.context = &disk;
	volume.start_lba = TEST_BASE_LBA;
	volume.sector_size = 512;
	volume.read = test_read;
	volume.write = 0;
	assert(boots_fs_mount(&filesystem, &volume, drivers, 1));
	assert(!strcmp(filesystem.driver->name, "fat12"));
	assert(boots_fs_open(&filesystem, "/kernel.bin", &file));
	assert(file.size == 5);
	assert(boots_file_read(&file, 0, buffer, 5));
	assert(!strcmp(buffer, "hello"));
	memset(buffer, 0, sizeof(buffer));
	assert(boots_fs_open(&filesystem, "/cmd/remacs.nap", &file));
	assert(boots_file_read(&file, 0, buffer, 5));
	assert(!strcmp(buffer, "remac"));
	assert(boots_fs_readdir(&filesystem, "/", 0, &entry));
	assert(!strcmp(entry.name, "KERNEL.BIN"));
	assert(boots_file_write_result(&file, 0, "x", 1) ==
	       BOOTS_FS_READ_ONLY);
	/* The 341 -> 342 chain exercises the sector-straddling entry. */
	assert(boots_fs_open(&filesystem, "/long.bin", &file));
	assert(file.size == 600);
	assert(boots_file_read(&file, 0, long_buffer, 600));
	assert(boots_file_contiguous_lba(&file, &lba));
	assert(lba == TEST_BASE_LBA + disk.data_lba + (341U - 2U));
}

static void test_fat12_write(void)
{
	const struct boots_filesystem_driver *const drivers[] = {
		&boots_fat12_driver,
	};
	struct test_disk disk;
	struct boots_volume volume;
	struct boots_filesystem filesystem;
	struct boots_file file;
	char buffer[7] = { 0 };

	make_fat12_disk(&disk);
	volume.context = &disk;
	volume.start_lba = TEST_BASE_LBA;
	volume.sector_size = 512;
	volume.read = test_read;
	volume.write = test_write;
	assert(boots_fs_mount(&filesystem, &volume, drivers, 1));
	assert(boots_fs_create_result(&filesystem, "/new.txt", &file) ==
	       BOOTS_FS_OK);
	assert(boots_file_write_result(&file, 0, "fat12!", 6) ==
	       BOOTS_FS_OK);
	assert(boots_file_flush_result(&file) == BOOTS_FS_OK);
	/* The first free cluster is 5; its packed entry must now be an
	 * end-of-chain marker and the neighbours must be untouched. */
	assert(fat12_get(disk.fat, 5) == 0xfff);
	assert(fat12_get(disk.fat, 4) == 0xfff);
	assert(fat12_get(disk.fat, 6) == 0);
	assert(boots_fs_open(&filesystem, "/new.txt", &file));
	assert(file.size == 6);
	assert(boots_file_read(&file, 0, buffer, 6));
	assert(!strcmp(buffer, "fat12!"));
	/* Truncation must free the chain again through the 12-bit
	 * read-modify-write path. */
	assert(boots_file_truncate_result(&file, 0) == BOOTS_FS_OK);
	assert(boots_file_flush_result(&file) == BOOTS_FS_OK);
	assert(fat12_get(disk.fat, 5) == 0);
}

static void test_probe_io_error(void)
{
	const struct boots_filesystem_driver *const drivers[] = {
		&boots_fat16_driver,
	};
	struct test_disk disk;
	struct boots_volume volume;
	struct boots_filesystem filesystem;

	make_disk(&disk, 512);
	disk.fail_reads = 1;
	volume.context = &disk;
	volume.start_lba = TEST_BASE_LBA;
	volume.sector_size = 512;
	volume.read = test_read;
	volume.write = 0;
	assert(boots_fs_mount_result(&filesystem, &volume, drivers, 1) ==
	       BOOTS_FS_IO_ERROR);
}

static void test_fat32_bpb_layout_rejected_for_fat16(void)
{
	const struct boots_filesystem_driver *const drivers[] = {
		&boots_fat16_driver,
	};
	struct test_disk disk;
	struct boots_volume volume;
	struct boots_filesystem filesystem;

	make_disk(&disk, 512);
	put16(disk.bpb + 22, 0);
	put32(disk.bpb + 36, 17);
	volume.context = &disk;
	volume.start_lba = TEST_BASE_LBA;
	volume.sector_size = 512;
	volume.read = test_read;
	volume.write = 0;
	assert(!boots_fs_mount(&filesystem, &volume, drivers, 1));
}

int main(void)
{
	test_fat16(512);
	test_fat16(1024);
	test_fat12();
	test_fat12_write();
	test_fat32_bpb_layout_rejected_for_fat16();
	test_probe_io_error();
	test_volume_write_contract();
	puts("Boots FAT12/FAT16 host tests: OK");
	return 0;
}
