/* X68000 partition table positive and corruption-path tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdio.h>
#include <string.h>
#include <kern/x68k-partition.h>

#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

/* The scan adapter is linked but not exercised in this pure decoder test. */
int disk_read(struct disk *d, uint64_t b, uint32_t n, void *p)
{ (void)d; (void)b; (void)n; (void)p; return -1; }

static void
put_be32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

static void
make_valid(uint8_t image[X68K_PARTITION_BOOT_BYTES])
{
	uint8_t *table, *entry;
	memset(image, 0, X68K_PARTITION_BOOT_BYTES);
	memcpy(image, "X68SCSI1", 8);
	table = image + 2048;
	memcpy(table, "X68K\0\0\0 ", 8);
	put_be32(table + 8, 32768U);
	put_be32(table + 12, 32768U);
	entry = table + 16;
	memcpy(entry, "ZEDBSD  ", 8);
	entry[8] = 0;
	entry[9] = 0;
	entry[10] = 0x08;
	entry[11] = 0x00; /* block1024 2048 -> LBA512 4096 */
	put_be32(entry + 12, 30720U);
	for (unsigned index = 1; index < 8; index++)
		table[16U + index * 16U + 8U] = 1U;
}

int
main(void)
{
	uint8_t image[X68K_PARTITION_BOOT_BYTES];
	struct partition entries[X68K_PARTITION_COUNT];
	int count;

	make_valid(image);
	count = x68k_partition_decode(image, sizeof(image), 65536U,
		entries, X68K_PARTITION_COUNT);
	CHECK(count == 8);
	CHECK(entries[0].p_index == 0U);
	CHECK(entries[0].p_start_block == 4096U);
	CHECK(entries[0].p_block_count == 61440U);
	CHECK((entries[0].p_flags & PARTITION_BOOTABLE) != 0);
	CHECK(strcmp(entries[0].p_label, "ZEDBSD") == 0);
	CHECK(entries[1].p_block_count == 0);

	image[0] = 'B';
	CHECK(x68k_partition_decode(image, sizeof(image), 65536U,
		entries, 8) < 0);
	make_valid(image);
	image[2048] = 'B';
	CHECK(x68k_partition_decode(image, sizeof(image), 65536U,
		entries, 8) < 0);
	make_valid(image);
	CHECK(x68k_partition_decode(image, sizeof(image) - 1U, 65536U,
		entries, 8) < 0);
	make_valid(image);
	put_be32(image + 2048 + 16 + 12, 0xffffffffU);
	CHECK(x68k_partition_decode(image, sizeof(image), 65536U,
		entries, 8) == 8);
	CHECK(entries[0].p_block_count == 0);
	make_valid(image);
	image[2048 + 16 + 9] = 0;
	image[2048 + 16 + 10] = 0;
	image[2048 + 16 + 11] = 3; /* overlaps the raw 4-KiB boot area */
	CHECK(x68k_partition_decode(image, sizeof(image), 65536U,
		entries, 8) == 8);
	CHECK(entries[0].p_block_count == 0);

	puts("X68k partition host tests passed");
	return 0;
}
