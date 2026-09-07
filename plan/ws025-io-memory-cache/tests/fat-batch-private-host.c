/* Exercise actual FAT transactions with uncertain media completion and mirrors. */
#include <stdio.h>
#include <stdlib.h>
#include "src/drivers/fs/fat.c"

#define REQUIRE(x) do { checks++; if (!(x)) { fprintf(stderr, "FAT batch line %u: %s\n", __LINE__, #x); abort(); } } while (0)
static uint8_t media[64 * 512], durable[64 * 512], original[64 * 512];
static unsigned checks, reads, writes, flushes, fail_read, fail_write, fail_write2, fail_flush;
static int write_then_error, fail_allocate;
static struct mount owner;
static int verify_lfn_order;
static void directory_cases(void);

void *kern_malloc(size_t size)
{ if (fail_allocate) { fail_allocate = 0; return NULL; } return malloc(size); }
void kern_free(void *p) { free(p); }
int disk_read(struct disk *disk, uint64_t first, uint32_t count, void *buffer)
{
	(void)disk;
	REQUIRE((first + count) * 512 <= sizeof(media));
	reads++;
	if (reads == fail_read) return EIO;
	memcpy(buffer, media + first * 512, count * 512);
	return 0;
}
int disk_read_direct(struct disk *disk, uint64_t first, uint32_t count, void *buffer)
{ return disk_read(disk, first, count, buffer); }
int disk_write_filesystem(struct disk *disk, uint64_t first, uint32_t count, const void *buffer)
{
	int failed;
	(void)disk;
	REQUIRE((first + count) * 512 <= sizeof(media));
	writes++;
	if (verify_lfn_order && first == 41 && ((const uint8_t *)buffer)[0] == 0x63) {
		REQUIRE(durable[40 * 512] == 0x51);
		REQUIRE(durable[41 * 512 - 1] == 0x51);
	}
	failed = writes == fail_write || writes == fail_write2;
	if (!failed || write_then_error) memcpy(media + first * 512, buffer, count * 512);
	return failed ? EIO : 0;
}
int disk_sync(struct disk *disk)
{
	(void)disk;
	flushes++;
	if (flushes == fail_flush) return EIO;
	memcpy(durable, media, sizeof(media));
	return 0;
}

static void prepare(struct fat_mount_state *fs, struct disk *disk, unsigned type)
{
	unsigned n;
	memset(fs, 0, sizeof(*fs));
	memset(disk, 0, sizeof(*disk));
	memset(&owner, 0, sizeof(owner));
	fs->owner = &owner;
	fs->disk = disk;
	fs->type = (uint8_t)type;
	fs->fat_start = 1;
	fs->fat_sectors = 16;
	fs->number_of_fats = 2;
	fs->cluster_count = 2000;
	fs->total_sectors = 64;
	for (n = 0; n < sizeof(media); n++) media[n] = (uint8_t)(n * 17U + n / 512U);
	memcpy(original, media, sizeof(media));
	memcpy(durable, media, sizeof(media));
	reads = writes = flushes = fail_read = fail_write = fail_write2 = fail_flush = 0;
	write_then_error = fail_allocate = verify_lfn_order = 0;
}

int main(void)
{
	struct fat_mount_state fs;
	struct disk disk;
	struct fat_entry_change changes[5];
	unsigned types[] = { ZEDBSD_FAT12, ZEDBSD_FAT16, ZEDBSD_FAT32 };
	unsigned type, mode, boundary, n;
	uint32_t value;
	int admitted, error;

	for (type = 0; type < 3; type++) {
		/* Include the packed FAT12 entry crossing byte 511. */
		changes[0].cluster = types[type] == ZEDBSD_FAT12 ? 341 : 2;
		changes[0].value = 0xabc;
		for (mode = 0; mode < 5; mode++) {
			for (boundary = 1; boundary <= 8; boundary++) {
				prepare(&fs, &disk, types[type]);
				if (mode == 0) fail_read = boundary;
				if (mode == 1 || mode == 2 || mode == 4) fail_write = boundary;
				if (mode == 2 || mode == 4) write_then_error = 1;
				if (mode == 3) fail_flush = boundary;
				if (mode == 4) fail_write2 = boundary + 1;
				error = fat_table_transaction(&fs, changes, 1, &admitted);
				fail_read = 0;
				REQUIRE(error == 0 || error == EIO);
				if (error != 0 && !fs.read_only) {
					REQUIRE(memcmp(media, original, sizeof(media)) == 0);
					REQUIRE(memcmp(durable, original, sizeof(durable)) == 0);
				}
				if (fs.read_only) REQUIRE(error != 0);
				if (error == 0) {
					for (n = 0; n < 2; n++) {
						REQUIRE(fat_raw_get_cluster_copy(&fs, changes[0].cluster, n, &value) == 0);
						REQUIRE(value == 0xabc);
					}
					REQUIRE(memcmp(media, durable, sizeof(media)) == 0);
				}
				REQUIRE(!fs.sector_cache_dirty);
				REQUIRE(owner.m_write_epoch.active == 0);
			}
		}

		/* Admission failure is distinguishable from an error after admission. */
		prepare(&fs, &disk, types[type]);
		fail_allocate = 1;
		REQUIRE(fat_table_transaction(&fs, changes, 1, &admitted) == ENOMEM);
		REQUIRE(!admitted && reads == 0 && writes == 0 && flushes == 0);
		fail_allocate = 1;
		REQUIRE(fat_raw_set_cluster(&fs, changes[0].cluster, 0xabc) == 0);
		REQUIRE(memcmp(media, durable, sizeof(media)) == 0);

		/* The allocation-free fallback restores each mirror's own differing old value. */
		prepare(&fs, &disk, types[type]);
		fail_allocate = 1;
		fail_write = 2;
		write_then_error = 1;
		REQUIRE(fat_raw_set_cluster(&fs, changes[0].cluster, 0xabc) == EIO);
		REQUIRE(!fs.read_only);
		REQUIRE(memcmp(media, original, sizeof(media)) == 0);
		REQUIRE(memcmp(durable, original, sizeof(durable)) == 0);
	}

	/* Refuse oversized sector sets without partially admitting a mutation. */
	prepare(&fs, &disk, ZEDBSD_FAT16);
	for (n = 0; n < 5; n++) { changes[n].cluster = 2 + n * 256; changes[n].value = 0; }
	REQUIRE(fat_table_transaction(&fs, changes, 5, &admitted) == E2BIG);
	REQUIRE(!admitted && writes == 0 && flushes == 0);
	REQUIRE(memcmp(media, original, sizeof(media)) == 0);
	directory_cases();
	printf("FAT transaction fault/ownership PASS %u checks\n", checks);
	return 0;
}


/* Verify grouped directory writes and the durable LFN-before-SFN boundary. */
static void directory_cases(void)
{
	struct fat_mount_state fs;
	struct disk disk;
	uint32_t lbas[17];
	uint16_t offsets[17];
	uint8_t entries[17][32];
	unsigned n, mode, boundary;
	int error;

	memset(entries, 0x51, sizeof(entries));
	memset(entries[16], 0x63, 32);
	for (n = 0; n < 17; n++) {
		lbas[n] = 40 + n / 16;
		offsets[n] = (uint16_t)((n % 16) * 32);
	}
	for (mode = 0; mode < 5; mode++) {
		for (boundary = 1; boundary <= 5; boundary++) {
			prepare(&fs, &disk, ZEDBSD_FAT32);
			verify_lfn_order = 1;
			if (mode == 0) fail_read = boundary;
			if (mode == 1 || mode == 2 || mode == 4) fail_write = boundary;
			if (mode == 2 || mode == 4) write_then_error = 1;
			if (mode == 3) fail_flush = boundary;
			if (mode == 4) fail_write2 = boundary + 1;
			error = fat_directory_transaction(&fs, lbas, offsets, entries, 17, 1);
			REQUIRE(error == 0 || error == EIO);
			if (error != 0 && !fs.read_only) {
				REQUIRE(memcmp(media, original, sizeof(media)) == 0);
				REQUIRE(memcmp(durable, original, sizeof(durable)) == 0);
			}
			if (error == 0) {
				REQUIRE(writes == 2);
				REQUIRE(memcmp(media + 40 * 512, entries, sizeof(entries)) == 0);
				REQUIRE(memcmp(media, durable, sizeof(media)) == 0);
			}
			REQUIRE(owner.m_write_epoch.active == 0);
			REQUIRE(!fs.sector_cache_dirty);
		}
	}

	/* A same-sector directory run needs only one physical publication. */
	prepare(&fs, &disk, ZEDBSD_FAT32);
	REQUIRE(fat_directory_transaction(&fs, lbas, offsets, entries, 2, 1) == 0);
	REQUIRE(writes == 1 && flushes == 2);
	REQUIRE(memcmp(media + 40 * 512, entries, 64) == 0);
	REQUIRE(memcmp(media + 40 * 512 + 64, original + 40 * 512 + 64, 448) == 0);
}

/* Synchronous media adapter retains the production context validation. */
int
disk_write_filesystem_context(struct disk *disk, uint64_t block, uint32_t count,
    const void *data, const struct io_context *context)
{
	int error = io_context_validate(context);
	return error != 0 ? error : disk_write_filesystem(disk, block, count, data);
}
