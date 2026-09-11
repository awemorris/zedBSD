/* WS013 disposable multi-ESP GPT image constructor. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
#define PARTITION_ALIGNMENT 2048U
#define MAX_PARTITIONS 8U
#define COPY_BUFFER_SIZE (64U * 1024U)

struct partition_source {
	const char *path;
	int descriptor;
	uint64_t byte_size;
	uint64_t first_lba;
	uint64_t last_lba;
};

static void
usage(FILE *stream, const char *program)
{
	fprintf(stream,
	    "usage: %s create OUTPUT FAT-IMAGE [FAT-IMAGE ...]\n"
	    "       %s --self-test\n"
	    "\n"
	    "Create a new protective-MBR/GPT disk.  Each block-aligned input\n"
	    "image becomes an EFI System Partition in argument order.  OUTPUT\n"
	    "must not already exist.  One to %u partitions are accepted.\n",
	    program, program, MAX_PARTITIONS);
}

static void
fail_errno(const char *operation, const char *path)
{
	if (path != NULL)
		fprintf(stderr, "%s: %s: %s\n", operation, path,
		    strerror(errno));
	else
		fprintf(stderr, "%s: %s\n", operation, strerror(errno));
	exit(1);
}

static void
fail_message(const char *message, const char *path)
{
	if (path != NULL)
		fprintf(stderr, "%s: %s\n", message, path);
	else
		fprintf(stderr, "%s\n", message);
	exit(2);
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

static uint64_t
align_up(uint64_t value, uint64_t alignment)
{
	uint64_t remainder = value % alignment;

	if (remainder == 0U)
		return value;
	if (value > UINT64_MAX - (alignment - remainder))
		fail_message("image layout overflows uint64_t", NULL);
	return value + alignment - remainder;
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
			fail_errno("pwrite", NULL);
		done += (size_t)amount;
	}
}

static void
read_exact(int descriptor, void *data, size_t size, uint64_t offset,
	const char *path)
{
	uint8_t *bytes = data;
	size_t done = 0U;

	while (done < size) {
		ssize_t amount = pread(descriptor, bytes + done, size - done,
		    (off_t)(offset + done));

		if (amount < 0 && errno == EINTR)
			continue;
		if (amount < 0)
			fail_errno("pread", path);
		if (amount == 0)
			fail_message("unexpected end of partition image", path);
		done += (size_t)amount;
	}
}

static void
copy_partition(int output, const struct partition_source *source)
{
	uint8_t buffer[COPY_BUFFER_SIZE];
	uint64_t copied = 0U;
	uint64_t destination = source->first_lba * BLOCK_SIZE;

	while (copied < source->byte_size) {
		uint64_t remaining = source->byte_size - copied;
		size_t amount = remaining < sizeof(buffer) ? (size_t)remaining :
		    sizeof(buffer);

		read_exact(source->descriptor, buffer, amount, copied,
		    source->path);
		write_exact(output, buffer, amount, destination + copied);
		copied += amount;
	}
}

static void
make_header(uint8_t header[BLOCK_SIZE], uint64_t here, uint64_t alternate,
	uint64_t first_usable, uint64_t last_usable, uint64_t table_lba,
	const uint8_t disk_guid[16U], uint32_t table_crc)
{
	memset(header, 0, BLOCK_SIZE);
	memcpy(header, "EFI PART", 8U);
	put32(header + 8U, 0x00010000U);
	put32(header + 12U, HEADER_SIZE);
	put64(header + 24U, here);
	put64(header + 32U, alternate);
	put64(header + 40U, first_usable);
	put64(header + 48U, last_usable);
	memcpy(header + 56U, disk_guid, 16U);
	put64(header + 72U, table_lba);
	put32(header + 80U, ENTRY_COUNT);
	put32(header + 84U, ENTRY_SIZE);
	put32(header + 88U, table_crc);
	put32(header + 16U, crc32(header, HEADER_SIZE));
}

static void
make_partition_entry(uint8_t *entry, const struct partition_source *source,
	unsigned number, uint32_t disk_seed)
{
	static const uint8_t efi_system_partition_guid[16U] = {
		0x28U, 0x73U, 0x2aU, 0xc1U, 0x1fU, 0xf8U, 0xd2U, 0x11U,
		0xbaU, 0x4bU, 0x00U, 0xa0U, 0xc9U, 0x3eU, 0xc9U, 0x3bU
	};
	static const char name_prefix[] = "WS013 FAT ";
	unsigned index;

	memcpy(entry, efi_system_partition_guid,
	    sizeof(efi_system_partition_guid));
	put32(entry + 16U, disk_seed ^ (0x13579bdfU + number));
	put32(entry + 20U, 0x2468ace0U + number);
	put32(entry + 24U, 0x10203040U + number);
	put32(entry + 28U, 0xa0b0c000U + number);
	put64(entry + 32U, source->first_lba);
	put64(entry + 40U, source->last_lba);
	for (index = 0U; index < sizeof(name_prefix) - 1U; index++)
		entry[56U + index * 2U] = (uint8_t)name_prefix[index];
	entry[56U + index * 2U] = (uint8_t)('0' + number);
}

static void
make_disk_guid(uint8_t guid[16U], const char *output_path,
	uint32_t *disk_seed)
{
	static const uint8_t suffix[12U] = {
		0x57U, 0x53U, 0x30U, 0x31U, 0x33U, 0x2dU,
		0x47U, 0x50U, 0x54U, 0x2dU, 0x30U, 0x31U
	};

	*disk_seed = crc32((const uint8_t *)output_path, strlen(output_path));
	if (*disk_seed == 0U)
		*disk_seed = 1U;
	put32(guid, *disk_seed);
	memcpy(guid + 4U, suffix, sizeof(suffix));
}

static int
self_test(void)
{
	static const uint8_t vector[] = "123456789";

	if (TABLE_BYTES % BLOCK_SIZE != 0U ||
	    crc32(vector, sizeof(vector) - 1U) != 0xcbf43926U ||
	    align_up(2048U, 2048U) != 2048U ||
	    align_up(2049U, 2048U) != 4096U) {
		fprintf(stderr, "WS013 GPT helper self-test failed\n");
		return 1;
	}
	puts("WS013 GPT helper self-test: PASS");
	return 0;
}

int
main(int argc, char **argv)
{
	struct partition_source sources[MAX_PARTITIONS];
	uint8_t mbr[BLOCK_SIZE];
	uint8_t primary_header[BLOCK_SIZE];
	uint8_t backup_header[BLOCK_SIZE];
	uint8_t table[TABLE_BYTES];
	uint8_t disk_guid[16U];
	uint64_t next_lba = PARTITION_ALIGNMENT;
	uint64_t backup_table_lba;
	uint64_t backup_header_lba;
	uint64_t total_blocks;
	uint64_t total_bytes;
	uint32_t disk_seed;
	uint32_t table_crc;
	uint32_t protective_size;
	unsigned partition_count;
	unsigned index;
	int output;

	if (argc == 2 && strcmp(argv[1], "--help") == 0) {
		usage(stdout, argv[0]);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
		return self_test();
	if (argc < 4 || strcmp(argv[1], "create") != 0) {
		usage(stderr, argv[0]);
		return 2;
	}
	partition_count = (unsigned)argc - 3U;
	if (partition_count > MAX_PARTITIONS)
		fail_message("too many partition images", NULL);

	memset(sources, 0, sizeof(sources));
	for (index = 0U; index < partition_count; index++) {
		struct stat status;
		uint64_t blocks;

		sources[index].path = argv[index + 3U];
		sources[index].descriptor = open(sources[index].path, O_RDONLY);
		if (sources[index].descriptor < 0)
			fail_errno("open", sources[index].path);
		if (fstat(sources[index].descriptor, &status) != 0)
			fail_errno("fstat", sources[index].path);
		if (!S_ISREG(status.st_mode) || status.st_size <= 0 ||
		    (uint64_t)status.st_size % BLOCK_SIZE != 0U)
			fail_message("partition image must be a nonempty, "
			    "block-aligned regular file", sources[index].path);
		sources[index].byte_size = (uint64_t)status.st_size;
		blocks = sources[index].byte_size / BLOCK_SIZE;
		sources[index].first_lba = align_up(next_lba,
		    PARTITION_ALIGNMENT);
		if (blocks - 1U > UINT64_MAX - sources[index].first_lba)
			fail_message("partition layout overflows uint64_t",
			    sources[index].path);
		sources[index].last_lba = sources[index].first_lba +
		    blocks - 1U;
		if (sources[index].last_lba == UINT64_MAX)
			fail_message("partition layout overflows uint64_t",
			    sources[index].path);
		next_lba = sources[index].last_lba + 1U;
	}

	backup_table_lba = align_up(next_lba, PARTITION_ALIGNMENT);
	if (backup_table_lba > UINT64_MAX - TABLE_BLOCKS)
		fail_message("GPT backup layout overflows uint64_t", NULL);
	backup_header_lba = backup_table_lba + TABLE_BLOCKS;
	if (backup_header_lba == UINT64_MAX)
		fail_message("disk layout overflows uint64_t", NULL);
	total_blocks = backup_header_lba + 1U;
	if (total_blocks > (uint64_t)INT64_MAX / BLOCK_SIZE)
		fail_message("disk image is too large for host offsets", NULL);
	total_bytes = total_blocks * BLOCK_SIZE;

	output = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (output < 0)
		fail_errno("create", argv[2]);
	if (ftruncate(output, (off_t)total_bytes) != 0)
		fail_errno("ftruncate", argv[2]);

	memset(table, 0, sizeof(table));
	make_disk_guid(disk_guid, argv[2], &disk_seed);
	for (index = 0U; index < partition_count; index++)
		make_partition_entry(table + index * ENTRY_SIZE, &sources[index],
		    index + 1U, disk_seed);
	table_crc = crc32(table, sizeof(table));
	make_header(primary_header, 1U, backup_header_lba, 34U,
	    backup_table_lba - 1U, 2U, disk_guid, table_crc);
	make_header(backup_header, backup_header_lba, 1U, 34U,
	    backup_table_lba - 1U, backup_table_lba, disk_guid, table_crc);

	memset(mbr, 0, sizeof(mbr));
	mbr[0x1beU + 4U] = 0xeeU;
	put32(mbr + 0x1beU + 8U, 1U);
	protective_size = total_blocks - 1U > UINT32_MAX ? UINT32_MAX :
	    (uint32_t)(total_blocks - 1U);
	put32(mbr + 0x1beU + 12U, protective_size);
	mbr[510U] = 0x55U;
	mbr[511U] = 0xaaU;

	write_exact(output, mbr, sizeof(mbr), 0U);
	write_exact(output, primary_header, sizeof(primary_header), BLOCK_SIZE);
	write_exact(output, table, sizeof(table), 2U * BLOCK_SIZE);
	for (index = 0U; index < partition_count; index++)
		copy_partition(output, &sources[index]);
	write_exact(output, table, sizeof(table),
	    backup_table_lba * BLOCK_SIZE);
	write_exact(output, backup_header, sizeof(backup_header),
	    backup_header_lba * BLOCK_SIZE);
	if (fsync(output) != 0)
		fail_errno("fsync", argv[2]);
	if (close(output) != 0)
		fail_errno("close", argv[2]);
	for (index = 0U; index < partition_count; index++)
		if (close(sources[index].descriptor) != 0)
			fail_errno("close", sources[index].path);

	printf("disk=%s blocks=%" PRIu64 " bytes=%" PRIu64 "\n", argv[2],
	    total_blocks, total_bytes);
	for (index = 0U; index < partition_count; index++)
		printf("partition=%u first-lba=%" PRIu64
		    " last-lba=%" PRIu64 " bytes=%" PRIu64 " source=%s\n",
		    index + 1U, sources[index].first_lba,
		    sources[index].last_lba, sources[index].byte_size,
		    sources[index].path);
	return 0;
}
