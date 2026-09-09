/* Actual FAT truncate: capacity admission, owned chains and rollback. */
#include "src/drivers/fs/fat.c"
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(x) do { checks++; if (!(x)) { fprintf(stderr, "growth line %d: %s\n", __LINE__, #x); abort(); } } while (0)
static uint8_t media[64 * 512], original[64 * 512];
static unsigned checks, reads, writes, barriers, fail_read, fail_write, fail_barrier;
static struct mount owner;
void *kern_malloc(size_t size) { return malloc(size); }
void kern_free(void *p) { free(p); }
void mutex_lock(struct mutex *p) { (void)p; }
void mutex_unlock(struct mutex *p) { (void)p; }
unsigned long spin_lock_irqsave(struct spinlock *p) { (void)p; return 0; }
void spin_unlock_irqrestore(struct spinlock *p, unsigned long f) { (void)p; (void)f; }
void io_stats_record(enum io_stat_event e, uint64_t b) { (void)e; (void)b; }
int disk_read(struct disk *d, uint64_t first, uint32_t count, void *p)
{
	(void)d; REQUIRE(first + count <= 64); reads++;
	if (reads == fail_read) return EIO;
	memcpy(p, media + first * 512, count * 512); return 0;
}
int disk_read_direct(struct disk *d, uint64_t f, uint32_t n, void *p)
{ return disk_read(d, f, n, p); }
int disk_write_filesystem(struct disk *d, uint64_t first, uint32_t count, const void *p)
{
	(void)d; REQUIRE(first + count <= 64); writes++;
	/* Even a reported failure can have reached the medium. */
	memcpy(media + first * 512, p, count * 512);
	return writes == fail_write ? EIO : 0;
}
int disk_write_filesystem_context(struct disk *d, uint64_t f, uint32_t n,
 const void *p, const struct io_context *c)
{ (void)c; return disk_write_filesystem(d, f, n, p); }
int disk_sync(struct disk *d)
{ (void)d; barriers++; return barriers == fail_barrier ? EIO : 0; }
static void entry(unsigned type, unsigned cluster, unsigned value)
{
	unsigned copy, offset;
	uint8_t *p;
	for (copy = 0; copy < 2; copy++) {
		p = media + (1 + copy) * 512;
		if (type == ZEDBSD_FAT12) {
			offset = cluster + cluster / 2;
			if (cluster & 1) {
				p[offset] = (p[offset] & 15) | ((value & 15) << 4);
				p[offset + 1] = value >> 4;
			} else {
				p[offset] = value;
				p[offset + 1] = (p[offset + 1] & 240) | ((value >> 8) & 15);
			}
		} else {
			offset = cluster * (type == ZEDBSD_FAT16 ? 2 : 4);
			p[offset] = value; p[offset + 1] = value >> 8;
			if (type == ZEDBSD_FAT32) { p[offset + 2] = value >> 16; p[offset + 3] = value >> 24; }
		}
	}
}
static void prepare(struct fat_mount_state *fs, struct disk *disk,
 struct fat_file_state *file, unsigned type, unsigned owned, unsigned size, unsigned free_count)
{
	unsigned n;
	memset(media, 0, sizeof(media)); memset(fs, 0, sizeof(*fs));
	memset(disk, 0, sizeof(*disk)); memset(file, 0, sizeof(*file));
	memset(&owner, 0, sizeof(owner));
	fs->disk = disk; fs->owner = &owner; fs->type = type;
	fs->fat_start = 1; fs->fat_sectors = 1; fs->number_of_fats = 2;
	fs->cluster_count = 16; fs->total_sectors = 64; fs->sectors_per_cluster = 1;
	fs->data_start = 16;
	for (n = 2; n < 18; n++) entry(type, n, fat_raw_end_of_chain(fs));
	for (n = 0; n < owned; n++) entry(type, 2 + n, n + 1 == owned ? fat_raw_end_of_chain(fs) : 3 + n);
	for (n = 0; n < free_count; n++) entry(type, 2 + owned + n, 0);
	file->mount = fs; file->size = size; file->first_cluster = owned ? 2 : 0;
	file->directory_lba = 4;
	memcpy(media + 4 * 512, "FILE    BIN", 11); media[4 * 512 + 11] = 0x20;
	fat_raw_put_dir_cluster(fs, media + 4 * 512, file->first_cluster);
	media[4 * 512 + 28] = size; media[4 * 512 + 29] = size >> 8;
	memset(media + 16 * 512, 0xa5, owned * 512);
	memcpy(original, media, sizeof(media));
	reads = writes = barriers = fail_read = fail_write = fail_barrier = 0;
}
int main(void)
{
	struct fat_mount_state fs;
	struct fat_file_state file;
	struct disk disk;
	unsigned types[] = {ZEDBSD_FAT12, ZEDBSD_FAT16, ZEDBSD_FAT32};
	unsigned t, n, mode, owned;
	int error;
	for (t = 0; t < 3; t++) {
		for (owned = 0; owned <= 2; owned++) {
			prepare(&fs, &disk, &file, types[t], owned, owned * 512, 1);
			REQUIRE(fat_raw_truncate(&file, (owned + 2) * 512) == ENOSPC);
			REQUIRE(writes == 0 && barriers == 0 && file.size == owned * 512);
			REQUIRE(memcmp(media, original, sizeof(media)) == 0);
		}
		prepare(&fs, &disk, &file, types[t], 1, 512, 0);
		REQUIRE(fat_raw_truncate(&file, 513) == ENOSPC && writes == 0);
		REQUIRE(memcmp(media, original, sizeof(media)) == 0);
		prepare(&fs, &disk, &file, types[t], 0, 0, 1);
		REQUIRE(fat_raw_truncate(&file, 512) == 0 && file.size == 512);
		prepare(&fs, &disk, &file, types[t], 0, 0, 16);
		REQUIRE(fat_raw_truncate(&file, 17 * 512) == ENOSPC && writes == 0 && reads == 0);
		prepare(&fs, &disk, &file, types[t], 2, 1, 0);
		REQUIRE(fat_raw_truncate(&file, 1024) == 0 && file.size == 1024);
		REQUIRE(media[16 * 512] == 0xa5);
		for (n = 1; n < 1024; n++) REQUIRE(media[16 * 512 + n] == 0);
		prepare(&fs, &disk, &file, types[t], 1, 512, 1);
		REQUIRE(fat_raw_truncate(&file, 1024) == 0 && file.size == 1024);
		REQUIRE(memcmp(media + 16 * 512, original + 16 * 512, 512) == 0);
		prepare(&fs, &disk, &file, types[t], 1, 513, 1);
		REQUIRE(fat_raw_truncate(&file, 1024) == EIO && writes == 0);
		prepare(&fs, &disk, &file, types[t], 1, 512, 1);
		entry(types[t], 2, 2);
		REQUIRE(fat_raw_truncate(&file, 1024) == EIO && writes == 0);
		prepare(&fs, &disk, &file, types[t], 0, 0, 1); fail_read = 1;
		REQUIRE(fat_raw_truncate(&file, 512) == EIO && writes == 0);
		for (mode = 0; mode < 2; mode++) for (n = 1; n <= 12; n++) {
			prepare(&fs, &disk, &file, types[t], 1, 512, 2);
			if (mode == 0) fail_write = n; else fail_barrier = n;
			error = fat_raw_truncate(&file, 1536);
			REQUIRE(error == 0 || error == EIO);
			REQUIRE(memcmp(media + 16 * 512, original + 16 * 512, 512) == 0);
			if (error != 0 && !fs.read_only) {
				REQUIRE(file.size == 512 && file.first_cluster == 2);
				REQUIRE(memcmp(media + 512, original + 512, 2 * 512) == 0);
			}
			REQUIRE(owner.m_write_epoch.active == 0);
		}
	}
	printf("FAT growth PASS %u checks\n", checks); return 0;
}
