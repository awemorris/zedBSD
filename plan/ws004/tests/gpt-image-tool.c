/*
 * WS004 p024 disposable GPT namespace constructor.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK_SIZE 512U
#define ENTRY_COUNT 128U
#define ENTRY_SIZE 128U
#define TABLE_BYTES (ENTRY_COUNT * ENTRY_SIZE)
#define TABLE_BLOCKS (TABLE_BYTES / BLOCK_SIZE)
#define HEADER_SIZE 92U

static void
fail(const char *operation)
{
	perror(operation);
	exit(1);
}

static void
put32(uint8_t *output, uint32_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
	output[2] = (uint8_t)(value >> 16);
	output[3] = (uint8_t)(value >> 24);
}

static void
put64(uint8_t *output, uint64_t value)
{
	put32(output, (uint32_t)value);
	put32(output + 4U, (uint32_t)(value >> 32));
}

static uint32_t
crc32(const uint8_t *data, size_t size)
{
	uint32_t crc = UINT32_MAX;

	while (size-- != 0U) {
		unsigned bit;

		crc ^= *data++;
		for (bit = 0U; bit < 8U; bit++)
			crc = (crc >> 1) ^
			    (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
	}
	return ~crc;
}

static void
write_exact(int descriptor, const void *data, size_t size, uint64_t offset)
{
	const uint8_t *bytes = data;
	size_t done = 0U;

	while (done < size) {
		ssize_t amount = pwrite(descriptor, bytes + done, size - done,
		    (off_t)(offset + done));

		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0)
			fail("pwrite");
		done += (size_t)amount;
	}
}

static void
make_header(uint8_t header[BLOCK_SIZE], uint64_t here, uint64_t alternate,
	uint64_t first_usable, uint64_t last_usable, uint64_t table_lba,
	uint32_t table_crc)
{
	static const uint8_t disk_guid[16U] = {
		0xefU, 0xcdU, 0xabU, 0x89U, 0x67U, 0x45U, 0x23U, 0x01U,
		0x80U, 0x81U, 0x82U, 0x83U, 0x84U, 0x85U, 0x86U, 0x87U
	};

	memset(header, 0, BLOCK_SIZE);
	memcpy(header, "EFI PART", 8U);
	put32(header + 8U, 0x00010000U);
	put32(header + 12U, HEADER_SIZE);
	put64(header + 24U, here);
	put64(header + 32U, alternate);
	put64(header + 40U, first_usable);
	put64(header + 48U, last_usable);
	memcpy(header + 56U, disk_guid, sizeof(disk_guid));
	put64(header + 72U, table_lba);
	put32(header + 80U, ENTRY_COUNT);
	put32(header + 84U, ENTRY_SIZE);
	put32(header + 88U, table_crc);
	put32(header + 16U, crc32(header, HEADER_SIZE));
}

static void
make_partition_entry(uint8_t table[TABLE_BYTES], uint64_t first,
	uint64_t last)
{
	static const uint8_t basic_data_guid[16U] = {
		0xa2U, 0xa0U, 0xd0U, 0xebU, 0xe5U, 0xb9U, 0x33U, 0x44U,
		0x87U, 0xc0U, 0x68U, 0xb6U, 0xb7U, 0x26U, 0x99U, 0xc7U
	};
	static const uint8_t unique_guid[16U] = {
		0x33U, 0x22U, 0x11U, 0x00U, 0x55U, 0x44U, 0x77U, 0x66U,
		0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU
	};
	static const char name[] = "ZEDBSD-NVME-TEST";
	unsigned index;

	memset(table, 0, TABLE_BYTES);
	memcpy(table, basic_data_guid, sizeof(basic_data_guid));
	memcpy(table + 16U, unique_guid, sizeof(unique_guid));
	put64(table + 32U, first);
	put64(table + 40U, last);
	for (index = 0U; index < sizeof(name) - 1U; index++)
		table[56U + index * 2U] = (uint8_t)name[index];
}

int
main(int argc, char **argv)
{
	uint8_t mbr[BLOCK_SIZE], primary[BLOCK_SIZE], backup[BLOCK_SIZE];
	uint8_t *table;
	struct stat status;
	const char *mode;
	uint64_t blocks, first_usable = 34U, last_usable, partition_first;
	uint64_t primary_table = 2U, backup_table;
	uint32_t table_crc, protective_size;
	int descriptor;

	if ((argc != 2 && argc != 3) || (argc == 3 &&
	    strcmp(argv[1], "valid") != 0 && strcmp(argv[1], "broken") != 0 &&
	    strcmp(argv[1], "primary-damaged") != 0 &&
	    strcmp(argv[1], "backup-damaged") != 0)) {
		fprintf(stderr,
		    "usage: %s [valid|broken|primary-damaged|backup-damaged] IMAGE\n",
		    argv[0]);
		return 2;
	}
	mode = argc == 2 ? "valid" : argv[1];
	descriptor = open(argv[argc - 1], O_RDWR);
	if (descriptor < 0)
		fail("open");
	if (fstat(descriptor, &status) != 0)
		fail("fstat");
	if (status.st_size < (off_t)(4096U * BLOCK_SIZE) ||
	    status.st_size % BLOCK_SIZE != 0) {
		fprintf(stderr, "image must be a block-aligned file of at least 2 MiB\n");
		return 2;
	}
	blocks = (uint64_t)status.st_size / BLOCK_SIZE;
	backup_table = blocks - 1U - TABLE_BLOCKS;
	last_usable = backup_table - 1U;
	partition_first = 2048U;
	if (partition_first > last_usable) {
		fprintf(stderr, "image is too small for the test partition\n");
		return 2;
	}
	table = calloc(1U, TABLE_BYTES);
	if (table == NULL)
		fail("calloc");
	make_partition_entry(table, partition_first, last_usable);
	table_crc = crc32(table, TABLE_BYTES);
	make_header(primary, 1U, blocks - 1U, first_usable, last_usable,
	    primary_table, table_crc);
	make_header(backup, blocks - 1U, 1U, first_usable, last_usable,
	    backup_table, table_crc);
	if (strcmp(mode, "broken") == 0 ||
	    strcmp(mode, "primary-damaged") == 0)
		primary[16U] ^= 1U;
	if (strcmp(mode, "broken") == 0 ||
	    strcmp(mode, "backup-damaged") == 0)
		backup[16U] ^= 1U;

	memset(mbr, 0, sizeof(mbr));
	mbr[510U] = 0x55U;
	mbr[511U] = 0xaaU;
	mbr[0x1beU + 4U] = 0xeeU;
	put32(mbr + 0x1beU + 8U, 1U);
	protective_size = blocks - 1U > UINT32_MAX ? UINT32_MAX :
	    (uint32_t)(blocks - 1U);
	put32(mbr + 0x1beU + 12U, protective_size);

	write_exact(descriptor, mbr, sizeof(mbr), 0U);
	write_exact(descriptor, primary, sizeof(primary), BLOCK_SIZE);
	write_exact(descriptor, table, TABLE_BYTES,
	    primary_table * BLOCK_SIZE);
	write_exact(descriptor, table, TABLE_BYTES,
	    backup_table * BLOCK_SIZE);
	write_exact(descriptor, backup, sizeof(backup),
	    (blocks - 1U) * BLOCK_SIZE);
	if (fsync(descriptor) != 0)
		fail("fsync");
	if (close(descriptor) != 0)
		fail("close");
	free(table);
	printf("GPT image: %s blocks=%llu partition=2048..%llu\n", mode,
	    (unsigned long long)blocks, (unsigned long long)last_usable);
	return 0;
}
