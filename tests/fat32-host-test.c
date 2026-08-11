/* zedBSD FAT32 host-side regression tests. */
#include "kern/fat32.h"
#include "kern/fat-lfn.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FAT_START 32U
#define FAT_SECTORS 600U
#define DATA_START (FAT_START + 2U * FAT_SECTORS)

static const uint8_t lfn_offsets[13] = {
	1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
};

struct test_disk {
	uint8_t bpb[512];
	uint8_t fat[512];
	uint8_t root[512];
	uint8_t data[512];
	uint8_t data2[512];
	uint8_t data3[512];
};

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static int read_sector(const void *context, uint32_t lba, void *buffer)
{
	const struct test_disk *disk = context;

	memset(buffer, 0, 512);
	if (lba == 0)
		memcpy(buffer, disk->bpb, 512);
	else if (lba == FAT_START || lba == FAT_START + FAT_SECTORS)
		memcpy(buffer, disk->fat, 512);
	else if (lba == DATA_START)
		memcpy(buffer, disk->root, 512);
	else if (lba == DATA_START + 1U)
		memcpy(buffer, disk->data, 512);
	else if (lba == DATA_START + 2U)
		memcpy(buffer, disk->data2, 512);
	else if (lba == DATA_START + 3U)
		memcpy(buffer, disk->data3, 512);
	return 1;
}

static int write_sector(void *context, uint32_t lba, const void *buffer)
{
	struct test_disk *disk = context;

	if (lba == FAT_START || lba == FAT_START + FAT_SECTORS)
		memcpy(disk->fat, buffer, 512);
	else if (lba == DATA_START)
		memcpy(disk->root, buffer, 512);
	else if (lba == DATA_START + 1U)
		memcpy(disk->data, buffer, 512);
	else if (lba == DATA_START + 2U)
		memcpy(disk->data2, buffer, 512);
	else if (lba == DATA_START + 3U)
		memcpy(disk->data3, buffer, 512);
	else
		return 0;
	return 1;
}

static uint8_t sfn_checksum(const uint8_t sfn[11])
{
	uint8_t sum = 0;
	unsigned i;
	for (i = 0; i < 11; i++)
		sum = (uint8_t)(((sum & 1U) << 7) | (sum >> 1)) + sfn[i];
	return sum;
}

static void make_lfn(uint8_t raw[32], const char *name,
		     const uint8_t sfn[11])
{
	unsigned i;
	size_t length = strlen(name);
	memset(raw, 0xff, 32);
	raw[0] = 0x41;
	raw[11] = 0x0f;
	raw[12] = 0;
	raw[13] = sfn_checksum(sfn);
	raw[26] = raw[27] = 0;
	for (i = 0; i < 13; i++) {
		uint16_t value = i < length ? (uint8_t)name[i] :
			(i == length ? 0 : 0xffffU);
		put16(raw + lfn_offsets[i], value);
	}
}

static void make_disk(struct test_disk *disk)
{
	uint8_t *entry;

	memset(disk, 0, sizeof(*disk));
	put16(disk->bpb + 11, 512);
	disk->bpb[13] = 1;
	put16(disk->bpb + 14, FAT_START);
	disk->bpb[16] = 2;
	put32(disk->bpb + 32, 70000);
	put32(disk->bpb + 36, FAT_SECTORS);
	put32(disk->bpb + 44, 2);
	put16(disk->bpb + 48, 1);
	put32(disk->fat + 0, 0x0ffffff8U);
	put32(disk->fat + 4, 0x0fffffffU);
	put32(disk->fat + 8, 0x0fffffffU);
	put32(disk->fat + 12, 0x0fffffffU);
	put32(disk->fat + 16, 0x0fffffffU);
	entry = disk->root;
	memcpy(entry, "HELLO   TXT", 11);
	entry[11] = 0x20;
	entry[12] = 0x18;
	put16(entry + 20, 0);
	put16(entry + 26, 3);
	put32(entry + 28, 5);
	memcpy(disk->data, "hello", 5);
	entry += 32;
	make_lfn(entry, "Read Me.txt", (const uint8_t *)"README~1TXT");
	entry += 32;
	memcpy(entry, "README~1TXT", 11);
	entry[11] = 0x20;
	put16(entry + 26, 4);
	put32(entry + 28, 6);
	memcpy(disk->data2, "second", 6);
}

int main(void)
{
	static const struct zedbsd_filesystem_driver *const drivers[] = {
		&zedbsd_fat32_driver,
	};
	struct test_disk disk;
	struct zedbsd_volume volume = { 0 };
	struct zedbsd_filesystem filesystem;
	struct zedbsd_file file;
	struct zedbsd_file created;
	struct zedbsd_dirent entry;
	char data[6] = { 0 };
	uint16_t units[FAT_LFN_MAX_UNITS];
	unsigned unit_count;

	assert(fat_utf8_casefold_equal("ReadMe.txt", "README.TXT"));
	assert(fat_utf8_casefold_equal("\xc3\x85.txt", "\xc3\xa5.txt"));
	assert(fat_utf8_casefold_equal("\xce\xa3", "\xcf\x82"));
	assert(fat_utf8_casefold_equal("\xe6\x97\xa5\xe6\x9c\xac",
				       "\xe6\x97\xa5\xe6\x9c\xac"));
	assert(!fat_utf8_casefold_equal("\xe6\x97\xa5", "\xe6\x9c\xac"));
	/* No Unicode normalization: U+00C5 differs from A + U+030A. */
	assert(!fat_utf8_casefold_equal("\xc3\x85", "A\xcc\x8a"));
	assert(!fat_utf8_casefold_equal("\xc0", "\xc0"));
	assert(fat_utf8_to_utf16("\xe6\x97\xa5\xe6\x9c\xac.txt", units,
				 &unit_count) && unit_count == 6);
	assert(fat_utf8_to_utf16("\xf0\x9f\x98\x80", units, &unit_count) &&
	       unit_count == 2 && units[0] == 0xd83d && units[1] == 0xde00);

	make_disk(&disk);
	volume.context = &disk;
	volume.sector_size = 512;
	volume.read = read_sector;
	volume.write = write_sector;
	assert(zedbsd_fs_mount(&filesystem, &volume, drivers, 1));
	assert(!strcmp(filesystem.driver->name, "fat32"));
	assert(zedbsd_fs_open(&filesystem, "/hello.txt", &file));
	assert(file.size == 5);
	assert(zedbsd_file_read(&file, 0, data, 5));
	assert(!strcmp(data, "hello"));
	assert(!zedbsd_fs_open(&filesystem, "/HELLO.TXT", &file));
	assert(zedbsd_fs_readdir(&filesystem, "/", 0, &entry));
	assert(!strcmp(entry.name, "hello.txt"));
	assert(zedbsd_fs_readdir(&filesystem, "/", 1, &entry));
	assert(!strcmp(entry.name, "Read Me.txt"));
	assert(zedbsd_fs_open(&filesystem, "/Read Me.txt", &file));
	assert(!zedbsd_fs_open(&filesystem, "/read me.txt", &file));
	assert(zedbsd_fs_create_result(&filesystem, "/read me.txt", &created) ==
	       ZEDBSD_FS_EXISTS);
	assert(zedbsd_fs_create_result(&filesystem, "/Camel Name.txt", &created) ==
	       ZEDBSD_FS_OK);
	assert(zedbsd_file_write_result(&created, 0, "created", 7) ==
	       ZEDBSD_FS_OK);
	assert(zedbsd_file_flush_result(&created) == ZEDBSD_FS_OK);
	assert(zedbsd_fs_readdir(&filesystem, "/", 2, &entry));
	assert(!strcmp(entry.name, "Camel Name.txt"));
	assert(zedbsd_fs_create_result(&filesystem,
		"/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt", &created) ==
	       ZEDBSD_FS_OK);
	assert(zedbsd_fs_readdir(&filesystem, "/", 3, &entry));
	assert(!strcmp(entry.name,
		       "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt"));
	assert(zedbsd_fs_create_result(&filesystem, "/\xc3\x85.txt", &created) ==
	       ZEDBSD_FS_OK);
	assert(zedbsd_fs_create_result(&filesystem, "/\xc3\xa5.txt", &created) ==
	       ZEDBSD_FS_EXISTS);
	zedbsd_fs_reset(&filesystem);
	assert(zedbsd_fs_mount(&filesystem, &volume, drivers, 1));
	assert(zedbsd_fs_open(&filesystem, "/Camel Name.txt", &file));
	memset(data, 0, sizeof(data));
	assert(zedbsd_file_read(&file, 0, data, 6));
	assert(!memcmp(data, "create", 6));
	assert(!zedbsd_fs_open(&filesystem, "/camel name.txt", &file));
	assert(zedbsd_fs_open(&filesystem,
		"/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt", &file));
	puts("zedBSD FAT32 host tests: OK");
	return 0;
}
