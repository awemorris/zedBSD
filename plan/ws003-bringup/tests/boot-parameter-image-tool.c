/* BR-T46 disposable disk-image layout helper. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum {
	SECTOR_SIZE = 512,
	MBR_TABLE_OFFSET = 0x1be,
	MBR_ENTRY_SIZE = 16,
	PC98_TABLE_LBA = 1,
	PC98_ENTRY_SIZE = 32,
	PC98_SECTORS = 17,
	GPT_HEADER_SIZE_MIN = 92,
	GPT_ENTRY_SIZE_MIN = 128,
	GPT_ENTRY_SIZE_MAX = 4096,
	GPT_ENTRY_COUNT_MAX = 4096,
	GPT_TABLE_BYTES_MAX = 16 * 1024 * 1024,
	SWAP_HEADER_SIZE = 64,
	SWAP_PAGE_SIZE = 4096,
	SWAP_CHECKSUM_OFFSET = 60
};

static void
fail(const char *message)
{
	fprintf(stderr, "boot-parameter-image-tool: %s\n", message);
	exit(1);
}

static void
fail_errno(const char *path)
{
	fprintf(stderr, "boot-parameter-image-tool: %s: %s\n", path,
	    strerror(errno));
	exit(1);
}

static uint16_t
get16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t
get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
get64(const uint8_t *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void
put16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void
put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void
put64(uint8_t *p, uint64_t value)
{
	put32(p, (uint32_t)value);
	put32(p + 4, (uint32_t)(value >> 32));
}

static int
all_zero(const uint8_t *bytes, size_t size)
{
	while (size-- != 0)
		if (*bytes++ != 0)
			return 0;
	return 1;
}

static uint32_t
gpt_crc32(const uint8_t *bytes, size_t size)
{
	uint32_t crc = UINT32_MAX;

	while (size-- != 0) {
		unsigned bit;

		crc ^= *bytes++;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
			    (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
	}
	return ~crc;
}

static uint64_t
parse_u64(const char *text, const char *name)
{
	char *end;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 0);
	if (errno != 0 || *text == '\0' || *end != '\0') {
		fprintf(stderr, "boot-parameter-image-tool: invalid %s: %s\n",
		    name, text);
		exit(2);
	}
	return (uint64_t)value;
}

static void
read_exact(int descriptor, uint64_t offset, void *buffer, size_t size,
	   const char *path)
{
	uint8_t *out = buffer;
	size_t done = 0;

	while (done != size) {
		ssize_t count = pread(descriptor, out + done, size - done,
		    (off_t)(offset + done));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			fail_errno(path);
		done += (size_t)count;
	}
}

static void
write_exact(int descriptor, uint64_t offset, const void *buffer, size_t size,
	    const char *path)
{
	const uint8_t *input = buffer;
	size_t done = 0;

	while (done != size) {
		ssize_t count = pwrite(descriptor, input + done, size - done,
		    (off_t)(offset + done));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			fail_errno(path);
		done += (size_t)count;
	}
}

static uint64_t
file_size(int descriptor, const char *path)
{
	struct stat status;

	if (fstat(descriptor, &status) != 0)
		fail_errno(path);
	if (status.st_size < 0)
		fail("negative file size");
	return (uint64_t)status.st_size;
}

static void
pc98_chs(uint8_t out[4], uint64_t lba, unsigned heads)
{
	uint64_t cylinder, remainder;
	unsigned head, sector;

	cylinder = lba / ((uint64_t)heads * PC98_SECTORS);
	remainder = lba % ((uint64_t)heads * PC98_SECTORS);
	head = (unsigned)(remainder / PC98_SECTORS);
	sector = (unsigned)(remainder % PC98_SECTORS);
	if (cylinder > UINT16_MAX)
		fail("PC-98 partition exceeds CHS range");
	out[0] = (uint8_t)sector;
	out[1] = (uint8_t)head;
	put16(out + 2, (uint16_t)cylinder);
}

static uint64_t
pc98_lba(const uint8_t in[4], unsigned heads)
{
	return ((uint64_t)get16(in + 2) * heads + in[1]) * PC98_SECTORS +
	    in[0];
}

static uint64_t
pc98_fat_end(int image, const uint8_t table[SECTOR_SIZE], unsigned heads,
	     uint64_t disk_blocks, const char *image_path)
{
	const uint8_t *entry = table;
	uint8_t boot[SECTOR_SIZE];
	uint64_t start, total, blocks;
	uint16_t bytes_per_sector;

	if (entry[0] != 0xa1 || entry[1] != 0x91)
		return UINT64_MAX;
	start = pc98_lba(entry + 8, heads);
	if (start >= disk_blocks)
		return UINT64_MAX;
	read_exact(image, start * SECTOR_SIZE, boot, sizeof(boot), image_path);
	bytes_per_sector = get16(boot + 11);
	total = get16(boot + 19);
	if (total == 0)
		total = get32(boot + 32);
	if (boot[510] != 0x55 || boot[511] != 0xaa ||
	    bytes_per_sector < SECTOR_SIZE ||
	    bytes_per_sector % SECTOR_SIZE != 0 || total == 0 ||
	    total > UINT64_MAX / bytes_per_sector)
		return UINT64_MAX;
	blocks = total * bytes_per_sector / SECTOR_SIZE;
	if (blocks == 0 || blocks > disk_blocks - start)
		return UINT64_MAX;
	return start + blocks - 1U;
}

static int
pc98_boot_entry_can_shorten(const uint8_t entry[PC98_ENTRY_SIZE],
			    uint64_t new_start, uint64_t disk_blocks,
			    unsigned heads, uint64_t fat_end)
{
	uint64_t entry_start = pc98_lba(entry + 8, heads);
	uint64_t entry_end = pc98_lba(entry + 12, heads);

	return entry[0] == 0xa1 && entry[1] == 0x91 && disk_blocks != 0 &&
	    entry_start < new_start && entry_end == disk_blocks - 1U &&
	    fat_end != UINT64_MAX && fat_end + 1U == new_start;
}

static int
region_overlaps(uint64_t left_start, uint64_t left_count,
		uint64_t right_start, uint64_t right_count)
{
	return left_count != 0 && right_count != 0 &&
	    left_start < right_start + right_count &&
	    right_start < left_start + left_count;
}

static void
copy_payload(int image, int payload, uint64_t image_offset,
	     uint64_t payload_size, const char *image_path,
	     const char *payload_path)
{
	uint8_t buffer[64 * 1024];
	uint64_t done = 0;

	while (done != payload_size) {
		size_t amount = sizeof(buffer);
		if ((uint64_t)amount > payload_size - done)
			amount = (size_t)(payload_size - done);
		read_exact(payload, done, buffer, amount, payload_path);
		write_exact(image, image_offset + done, buffer, amount,
		    image_path);
		done += amount;
	}
}

static uint32_t
swap_checksum(const uint8_t header[SWAP_HEADER_SIZE])
{
	uint32_t value = 2166136261U;
	unsigned index;

	for (index = 0; index < SWAP_HEADER_SIZE; index++) {
		uint8_t byte = index >= SWAP_CHECKSUM_OFFSET ? 0 : header[index];
		value = (value ^ byte) * 16777619U;
	}
	return value;
}

static void
make_swap_v2_header(uint8_t header[SWAP_HEADER_SIZE], uint64_t bytes)
{
	uint64_t slots;

	if (bytes < 2U * SWAP_PAGE_SIZE || bytes % SWAP_PAGE_SIZE != 0)
		fail("swap partition must contain complete 4096-byte pages");
	slots = bytes / SWAP_PAGE_SIZE - 1U;
	memset(header, 0, SWAP_HEADER_SIZE);
	memcpy(header, "ZEDSWAP2", 8);
	put16(header + 8, 2);
	put16(header + 10, SWAP_HEADER_SIZE);
	put32(header + 12, SWAP_PAGE_SIZE);
	put64(header + 16, bytes);
	put64(header + 24, slots);
	put32(header + SWAP_CHECKSUM_OFFSET, swap_checksum(header));
}

static void
add_mbr_partition(uint8_t sector[SECTOR_SIZE], unsigned index,
		  uint8_t type, uint64_t start, uint64_t blocks,
		  uint64_t disk_blocks)
{
	uint8_t *entry;
	unsigned previous;

	if (sector[510] != 0x55 || sector[511] != 0xaa)
		fail("image has no MBR signature");
	if (index == 0 || index > 4 || start > UINT32_MAX ||
	    blocks > UINT32_MAX)
		fail("invalid MBR partition geometry");
	entry = sector + MBR_TABLE_OFFSET + (index - 1U) * MBR_ENTRY_SIZE;
	if (entry[4] != 0 || get32(entry + 8) != 0 || get32(entry + 12) != 0)
		fail("target MBR entry is not empty");
	for (previous = 0; previous < 4; previous++) {
		const uint8_t *candidate =
		    sector + MBR_TABLE_OFFSET + previous * MBR_ENTRY_SIZE;
		uint64_t candidate_start = get32(candidate + 8);
		uint64_t candidate_count = get32(candidate + 12);

		if (previous == index - 1U || candidate[4] == 0 ||
		    candidate[4] == 0xee)
			continue;
		if (region_overlaps(start, blocks, candidate_start,
		    candidate_count))
			fail("new MBR partition overlaps an existing partition");
	}
	if (start + blocks > disk_blocks)
		fail("new MBR partition exceeds the image");
	memset(entry, 0, MBR_ENTRY_SIZE);
	entry[1] = entry[5] = 0xfe;
	entry[2] = entry[6] = 0xff;
	entry[3] = entry[7] = 0xff;
	entry[4] = type;
	put32(entry + 8, (uint32_t)start);
	put32(entry + 12, (uint32_t)blocks);
}

struct gpt_copy {
	uint8_t header[SECTOR_SIZE];
	uint64_t current_lba;
	uint64_t alternate_lba;
	uint64_t first_usable;
	uint64_t last_usable;
	uint64_t table_lba;
	uint64_t table_bytes;
	uint32_t entry_count;
	uint32_t entry_size;
};

static void
read_gpt_copy(int image, uint64_t header_lba, uint64_t alternate_lba,
	uint64_t disk_blocks, int primary, struct gpt_copy *copy,
	const char *image_path)
{
	uint32_t header_size, stored_crc;
	uint64_t table_blocks;

	read_exact(image, header_lba * SECTOR_SIZE, copy->header,
	    sizeof(copy->header), image_path);
	if (memcmp(copy->header, "EFI PART", 8) != 0 ||
	    get32(copy->header + 8) != 0x00010000U)
		fail("invalid GPT signature or revision");
	header_size = get32(copy->header + 12);
	if (header_size < GPT_HEADER_SIZE_MIN || header_size > SECTOR_SIZE ||
	    get32(copy->header + 20) != 0)
		fail("invalid GPT header size or reserved field");
	stored_crc = get32(copy->header + 16);
	put32(copy->header + 16, 0);
	if (gpt_crc32(copy->header, header_size) != stored_crc)
		fail("invalid GPT header CRC");
	put32(copy->header + 16, stored_crc);

	copy->current_lba = get64(copy->header + 24);
	copy->alternate_lba = get64(copy->header + 32);
	copy->first_usable = get64(copy->header + 40);
	copy->last_usable = get64(copy->header + 48);
	copy->table_lba = get64(copy->header + 72);
	copy->entry_count = get32(copy->header + 80);
	copy->entry_size = get32(copy->header + 84);
	if (copy->current_lba != header_lba ||
	    copy->alternate_lba != alternate_lba ||
	    copy->first_usable < 2U ||
	    copy->first_usable > copy->last_usable ||
	    copy->last_usable >= disk_blocks - 1U ||
	    all_zero(copy->header + 56, 16))
		fail("invalid GPT header geometry");
	if (copy->entry_count == 0 ||
	    copy->entry_count > GPT_ENTRY_COUNT_MAX ||
	    copy->entry_size < GPT_ENTRY_SIZE_MIN ||
	    copy->entry_size > GPT_ENTRY_SIZE_MAX ||
	    copy->entry_size % GPT_ENTRY_SIZE_MIN != 0 ||
	    copy->entry_count > GPT_TABLE_BYTES_MAX / copy->entry_size)
		fail("unsupported GPT entry-array geometry");
	copy->table_bytes =
	    (uint64_t)copy->entry_count * copy->entry_size;
	table_blocks = (copy->table_bytes + SECTOR_SIZE - 1U) / SECTOR_SIZE;
	if (copy->table_lba >= disk_blocks ||
	    table_blocks > disk_blocks - copy->table_lba)
		fail("GPT entry array exceeds the image");
	if (primary) {
		if (copy->table_lba < 2U ||
		    copy->table_lba + table_blocks > copy->first_usable)
			fail("primary GPT entry array overlaps usable space");
	} else if (copy->table_lba <= copy->last_usable ||
	    copy->table_lba + table_blocks > copy->current_lba) {
		fail("backup GPT entry array overlaps usable space");
	}
}

static void
make_gpt_partition_entry(uint8_t *entry, uint32_t entry_size,
	unsigned index, int swap, uint64_t start, uint64_t blocks)
{
	/* FreeBSD swap and UFS partition type GUIDs, in GPT byte order. */
	static const uint8_t swap_type[16] = {
		0xb5, 0x7c, 0x6e, 0x51, 0xcf, 0x6e, 0xd6, 0x11,
		0x8f, 0xf8, 0x00, 0x02, 0x2d, 0x09, 0x71, 0x2b
	};
	static const uint8_t ufs_type[16] = {
		0xb6, 0x7c, 0x6e, 0x51, 0xcf, 0x6e, 0xd6, 0x11,
		0x8f, 0xf8, 0x00, 0x02, 0x2d, 0x09, 0x71, 0x2b
	};
	static const char ufs_name[] = "BR-T46 UFS";
	static const char swap_name[] = "BR-T46 SWAP";
	const char *name = swap ? swap_name : ufs_name;
	unsigned name_index;

	memset(entry, 0, entry_size);
	memcpy(entry, swap ? swap_type : ufs_type, 16);
	/* Deterministic test-only unique GUID, distinct for kind/index/geometry. */
	put32(entry + 16, 0x54343642U ^ (uint32_t)start);
	put32(entry + 20, 0x47505400U | (index & 0xffU));
	put32(entry + 24, (uint32_t)(start >> 32) ^ (uint32_t)blocks);
	put32(entry + 28, 0x80000000U ^ (uint32_t)(blocks >> 32) ^
	    (swap ? 0x53574150U : 0x55465300U));
	put64(entry + 32, start);
	put64(entry + 40, start + blocks - 1U);
	for (name_index = 0; name[name_index] != '\0'; name_index++)
		entry[56U + name_index * 2U] = (uint8_t)name[name_index];
}

static void
update_gpt_header_crc(struct gpt_copy *copy, uint32_t table_crc)
{
	uint32_t header_size = get32(copy->header + 12);

	put32(copy->header + 88, table_crc);
	put32(copy->header + 16, 0);
	put32(copy->header + 16, gpt_crc32(copy->header, header_size));
}

static void
add_gpt_partition(int image, unsigned index, int swap, uint64_t start,
	uint64_t blocks, uint64_t disk_blocks, const char *image_path)
{
	struct gpt_copy primary, backup;
	uint8_t *primary_table, *backup_table, *entry;
	uint64_t end = start + blocks - 1U;
	uint32_t table_crc;
	unsigned existing;

	read_gpt_copy(image, 1U, disk_blocks - 1U, disk_blocks, 1, &primary,
	    image_path);
	read_gpt_copy(image, disk_blocks - 1U, 1U, disk_blocks, 0, &backup,
	    image_path);
	if (primary.first_usable != backup.first_usable ||
	    primary.last_usable != backup.last_usable ||
	    primary.entry_count != backup.entry_count ||
	    primary.entry_size != backup.entry_size ||
	    primary.table_bytes != backup.table_bytes ||
	    memcmp(primary.header + 56, backup.header + 56, 16) != 0)
		fail("primary and backup GPT headers disagree");
	if (index == 0 || index > primary.entry_count || blocks == 0 ||
	    start < primary.first_usable || end > primary.last_usable)
		fail("new GPT partition exceeds usable space");

	primary_table = malloc((size_t)primary.table_bytes);
	backup_table = malloc((size_t)backup.table_bytes);
	if (primary_table == NULL || backup_table == NULL)
		fail("cannot allocate GPT entry arrays");
	read_exact(image, primary.table_lba * SECTOR_SIZE, primary_table,
	    (size_t)primary.table_bytes, image_path);
	read_exact(image, backup.table_lba * SECTOR_SIZE, backup_table,
	    (size_t)backup.table_bytes, image_path);
	if (gpt_crc32(primary_table, (size_t)primary.table_bytes) !=
	    get32(primary.header + 88) ||
	    gpt_crc32(backup_table, (size_t)backup.table_bytes) !=
	    get32(backup.header + 88) ||
	    memcmp(primary_table, backup_table,
	    (size_t)primary.table_bytes) != 0)
		fail("primary and backup GPT entry arrays disagree");
	entry = primary_table + (index - 1U) * primary.entry_size;
	if (!all_zero(entry, primary.entry_size))
		fail("target GPT entry is not empty");
	make_gpt_partition_entry(entry, primary.entry_size, index, swap, start,
	    blocks);
	for (existing = 0; existing < primary.entry_count; existing++) {
		const uint8_t *candidate =
		    primary_table + existing * primary.entry_size;
		uint64_t candidate_start, candidate_end;

		if (existing == index - 1U || all_zero(candidate, 16))
			continue;
		candidate_start = get64(candidate + 32);
		candidate_end = get64(candidate + 40);
		if (candidate_start > candidate_end ||
		    candidate_start < primary.first_usable ||
		    candidate_end > primary.last_usable)
			fail("existing GPT partition has invalid geometry");
		if (region_overlaps(start, blocks, candidate_start,
		    candidate_end - candidate_start + 1U))
			fail("new GPT partition overlaps an existing partition");
		if (memcmp(entry + 16, candidate + 16, 16) == 0)
			fail("new GPT partition GUID is not unique");
	}
	memcpy(backup_table, primary_table, (size_t)primary.table_bytes);
	table_crc = gpt_crc32(primary_table, (size_t)primary.table_bytes);
	update_gpt_header_crc(&primary, table_crc);
	update_gpt_header_crc(&backup, table_crc);
	write_exact(image, primary.table_lba * SECTOR_SIZE, primary_table,
	    (size_t)primary.table_bytes, image_path);
	write_exact(image, backup.table_lba * SECTOR_SIZE, backup_table,
	    (size_t)backup.table_bytes, image_path);
	write_exact(image, primary.current_lba * SECTOR_SIZE, primary.header,
	    sizeof(primary.header), image_path);
	write_exact(image, backup.current_lba * SECTOR_SIZE, backup.header,
	    sizeof(backup.header), image_path);
	free(backup_table);
	free(primary_table);
}

static void
add_pc98_partition(uint8_t table[SECTOR_SIZE], unsigned index,
		   int swap, uint64_t start, uint64_t blocks,
		   uint64_t disk_blocks, unsigned heads,
		   uint64_t boot_filesystem_end)
{
	uint8_t *entry;
	unsigned previous;

	if (index == 0 || index > SECTOR_SIZE / PC98_ENTRY_SIZE || blocks == 0 ||
	    start + blocks > disk_blocks)
		fail("invalid PC-98 partition geometry");
	entry = table + (index - 1U) * PC98_ENTRY_SIZE;
	if (entry[0] != 0)
		fail("target PC-98 partition entry is not empty");
	for (previous = 1; previous < index; previous++) {
		uint8_t *candidate =
		    table + (previous - 1U) * PC98_ENTRY_SIZE;
		uint64_t candidate_start, candidate_end;

		if (candidate[0] == 0)
			continue;
		candidate_start = pc98_lba(candidate + 8, heads);
		candidate_end = pc98_lba(candidate + 12, heads);
		if (candidate_end < candidate_start)
			fail("invalid existing PC-98 partition geometry");
		/* zedimage's single-partition PC-98 layout describes the boot entry
		 * through the end of the medium even though the FAT BPB ends at the
		 * requested partition start.  Make that implicit boundary explicit
		 * before adding a test-only second partition. */
		if (previous == 1U && pc98_boot_entry_can_shorten(candidate,
		    start, disk_blocks, heads, boot_filesystem_end)) {
			pc98_chs(candidate + 12, start - 1U, heads);
			candidate_end = start - 1U;
		}
		if (region_overlaps(start, blocks, candidate_start,
		    candidate_end - candidate_start + 1U))
			fail("new PC-98 partition overlaps an existing partition");
	}
	memset(entry, 0, PC98_ENTRY_SIZE);
	entry[0] = swap ? 0x22 : 0x21;
	entry[1] = swap ? 0x02 : 0x01;
	pc98_chs(entry + 4, start, heads);
	pc98_chs(entry + 8, start, heads);
	pc98_chs(entry + 12, start + blocks - 1U, heads);
	memset(entry + 16, ' ', 16);
	memcpy(entry + 16, swap ? "SWAP" : "ROOT", 4);
}

static void
add_partition(int argc, char **argv)
{
	const char *machine = NULL, *kind = NULL, *payload_path = NULL;
	const char *image_path = NULL;
	uint64_t start = 0;
	unsigned index = 0;
	int argument, image, payload, swap;
	uint64_t image_bytes, payload_bytes, disk_blocks, blocks;
	uint8_t sector[SECTOR_SIZE];

	for (argument = 2; argument < argc; argument++) {
		if (!strcmp(argv[argument], "--machine") && argument + 1 < argc)
			machine = argv[++argument];
		else if (!strcmp(argv[argument], "--kind") &&
		    argument + 1 < argc)
			kind = argv[++argument];
		else if (!strcmp(argv[argument], "--index") &&
		    argument + 1 < argc)
			index = (unsigned)parse_u64(argv[++argument], "index");
		else if (!strcmp(argv[argument], "--start-lba") &&
		    argument + 1 < argc)
			start = parse_u64(argv[++argument], "start LBA");
		else if (!strcmp(argv[argument], "--payload") &&
		    argument + 1 < argc)
			payload_path = argv[++argument];
		else if (argv[argument][0] != '-' && image_path == NULL)
			image_path = argv[argument];
		else
			fail("invalid add-partition argument");
	}
	if (machine == NULL || kind == NULL || payload_path == NULL ||
	    image_path == NULL || index == 0 || start == 0)
		fail("add-partition requires machine, kind, index, start, payload, image");
	if (strcmp(machine, "pcat") != 0 && strcmp(machine, "pc98") != 0)
		fail("machine must be pcat or pc98");
	swap = strcmp(kind, "swap") == 0;
	if (!swap && strcmp(kind, "ufs") != 0)
		fail("kind must be swap or ufs");
	image = open(image_path, O_RDWR);
	if (image < 0)
		fail_errno(image_path);
	payload = open(payload_path, O_RDONLY);
	if (payload < 0)
		fail_errno(payload_path);
	image_bytes = file_size(image, image_path);
	payload_bytes = file_size(payload, payload_path);
	if (image_bytes == 0 || image_bytes % SECTOR_SIZE != 0 ||
	    payload_bytes == 0 || payload_bytes % SECTOR_SIZE != 0)
		fail("image and payload must be positive sector multiples");
	disk_blocks = image_bytes / SECTOR_SIZE;
	blocks = payload_bytes / SECTOR_SIZE;
	if (start > disk_blocks || blocks > disk_blocks - start)
		fail("payload partition exceeds image");
	if (!strcmp(machine, "pcat")) {
		uint8_t gpt[SECTOR_SIZE];
		int has_gpt;

		read_exact(image, 0, sector, sizeof(sector), image_path);
		add_mbr_partition(sector, index, swap ? 0x82 : 0xa5, start,
		    blocks, disk_blocks);
		read_exact(image, SECTOR_SIZE, gpt, sizeof(gpt), image_path);
		has_gpt = memcmp(gpt, "EFI PART", 8) == 0;
		if (has_gpt)
			add_gpt_partition(image, index, swap, start, blocks,
			    disk_blocks, image_path);
		write_exact(image, 0, sector, sizeof(sector), image_path);
	} else {
		unsigned heads = image_bytes <= 20ULL * 1024ULL * 1024ULL ? 4 : 8;
		uint64_t boot_filesystem_end;
		read_exact(image, PC98_TABLE_LBA * SECTOR_SIZE, sector,
		    sizeof(sector), image_path);
		boot_filesystem_end = pc98_fat_end(image, sector, heads,
		    disk_blocks, image_path);
		add_pc98_partition(sector, index, swap, start, blocks,
		    disk_blocks, heads, boot_filesystem_end);
		write_exact(image, PC98_TABLE_LBA * SECTOR_SIZE, sector,
		    sizeof(sector), image_path);
	}
	copy_payload(image, payload, start * SECTOR_SIZE, payload_bytes,
	    image_path, payload_path);
	if (fsync(image) != 0)
		fail_errno(image_path);
	if (close(payload) != 0 || close(image) != 0)
		fail_errno(image_path);
	printf("partition=%u kind=%s start=%llu blocks=%llu\n", index, kind,
	    (unsigned long long)start, (unsigned long long)blocks);
}

static void
stamp_swap_v2(int argc, char **argv)
{
	const char *image_path = NULL;
	uint8_t sector[SECTOR_SIZE], header[SWAP_HEADER_SIZE];
	uint64_t image_bytes, disk_blocks, start, blocks, bytes;
	unsigned index = 0;
	int argument, image;

	for (argument = 2; argument < argc; argument++) {
		if (!strcmp(argv[argument], "--partition-index") &&
		    argument + 1 < argc)
			index = (unsigned)parse_u64(argv[++argument],
			    "partition index");
		else if (argv[argument][0] != '-' && image_path == NULL)
			image_path = argv[argument];
		else
			fail("invalid stamp-swap-v2 argument");
	}
	if (index == 0 || index > 4 || image_path == NULL)
		fail("stamp-swap-v2 requires an MBR partition index and image");
	image = open(image_path, O_RDWR);
	if (image < 0)
		fail_errno(image_path);
	image_bytes = file_size(image, image_path);
	if (image_bytes == 0 || image_bytes % SECTOR_SIZE != 0)
		fail("image must be a positive sector multiple");
	disk_blocks = image_bytes / SECTOR_SIZE;
	read_exact(image, 0, sector, sizeof(sector), image_path);
	if (sector[510] != 0x55 || sector[511] != 0xaa)
		fail("image has no MBR signature");
	{
		const uint8_t *entry =
		    sector + MBR_TABLE_OFFSET + (index - 1U) * MBR_ENTRY_SIZE;
		uint8_t type = entry[4];

		start = get32(entry + 8);
		blocks = get32(entry + 12);
		if (type == 0 || type == 0x05 || type == 0x0f || type == 0x85 ||
		    type == 0xee || start == 0 || blocks == 0 ||
		    start > disk_blocks || blocks > disk_blocks - start)
			fail("target is not a bounded MBR data partition");
	}
	bytes = blocks * SECTOR_SIZE;
	make_swap_v2_header(header, bytes);
	write_exact(image, start * SECTOR_SIZE, header, sizeof(header),
	    image_path);
	if (fsync(image) != 0 || close(image) != 0)
		fail_errno(image_path);
	printf("partition=%u swap=v2 start=%llu bytes=%llu slots=%llu\n", index,
	    (unsigned long long)start, (unsigned long long)bytes,
	    (unsigned long long)(bytes / SWAP_PAGE_SIZE - 1U));
}

static uint32_t
parse_uuid(const char *text)
{
	char compact[9];
	char *end;
	unsigned long value;
	unsigned source = 0, target = 0;

	while (text[source] != '\0') {
		char byte = text[source++];
		if (byte == '-')
			continue;
		if (target == 8 || !((byte >= '0' && byte <= '9') ||
		    (byte >= 'a' && byte <= 'f') ||
		    (byte >= 'A' && byte <= 'F')))
			fail("FAT UUID must contain eight hexadecimal digits");
		compact[target++] = byte;
	}
	if (target != 8)
		fail("FAT UUID must contain eight hexadecimal digits");
	compact[8] = '\0';
	errno = 0;
	value = strtoul(compact, &end, 16);
	if (errno != 0 || *end != '\0' || value > UINT32_MAX)
		fail("invalid FAT UUID");
	return (uint32_t)value;
}

static void
set_fat_uuid(int argc, char **argv)
{
	const char *uuid_text = NULL, *image_path = NULL;
	uint64_t partition_lba = 0;
	int argument, image;
	uint8_t boot[SECTOR_SIZE];
	uint32_t sectors, fatsz, root_sectors, data_sectors, clusters, serial;
	unsigned serial_offset;

	for (argument = 2; argument < argc; argument++) {
		if (!strcmp(argv[argument], "--partition-lba") &&
		    argument + 1 < argc)
			partition_lba =
			    parse_u64(argv[++argument], "partition LBA");
		else if (!strcmp(argv[argument], "--uuid") &&
		    argument + 1 < argc)
			uuid_text = argv[++argument];
		else if (argv[argument][0] != '-' && image_path == NULL)
			image_path = argv[argument];
		else
			fail("invalid set-fat-uuid argument");
	}
	if (partition_lba == 0 || uuid_text == NULL || image_path == NULL)
		fail("set-fat-uuid requires partition LBA, UUID, and image");
	serial = parse_uuid(uuid_text);
	image = open(image_path, O_RDWR);
	if (image < 0)
		fail_errno(image_path);
	read_exact(image, partition_lba * SECTOR_SIZE, boot, sizeof(boot),
	    image_path);
	if (boot[510] != 0x55 || boot[511] != 0xaa || get16(boot + 11) != 512 ||
	    boot[13] == 0 || boot[16] == 0)
		fail("target is not a supported 512-byte FAT filesystem");
	sectors = get16(boot + 19);
	if (sectors == 0)
		sectors = get32(boot + 32);
	fatsz = get16(boot + 22);
	if (fatsz == 0)
		fatsz = get32(boot + 36);
	root_sectors = ((uint32_t)get16(boot + 17) * 32U + 511U) / 512U;
	if (sectors <= get16(boot + 14) + (uint32_t)boot[16] * fatsz +
	    root_sectors)
		fail("invalid FAT geometry");
	data_sectors = sectors - (get16(boot + 14) +
	    (uint32_t)boot[16] * fatsz + root_sectors);
	clusters = data_sectors / boot[13];
	serial_offset = clusters < 65525U ? 39U : 67U;
	if (boot[serial_offset - 1U] != 0x29)
		fail("FAT volume has no extended serial field");
	put32(boot + serial_offset, serial);
	write_exact(image, partition_lba * SECTOR_SIZE, boot, sizeof(boot),
	    image_path);
	/* Give an auxiliary clone a distinct MBR PARTUUID as well. */
	read_exact(image, 0, boot, sizeof(boot), image_path);
	if (boot[510] == 0x55 && boot[511] == 0xaa) {
		put32(boot + 0x1b8, serial ^ 0x5a425344U);
		write_exact(image, 0, boot, sizeof(boot), image_path);
	}
	if (fsync(image) != 0 || close(image) != 0)
		fail_errno(image_path);
	printf("fat_uuid=%04X-%04X partition_lba=%llu\n",
	    (unsigned)(serial >> 16), (unsigned)(serial & 0xffffU),
	    (unsigned long long)partition_lba);
}

static void
self_test(void)
{
	uint8_t mbr[SECTOR_SIZE] = {0};
	uint8_t table[SECTOR_SIZE] = {0};
	uint8_t gpt_entry[GPT_ENTRY_SIZE_MIN];
	uint8_t swap_header[SWAP_HEADER_SIZE];
	struct gpt_copy gpt = {0};
	uint8_t *entry;
	uint32_t header_crc;

	mbr[510] = 0x55;
	mbr[511] = 0xaa;
	entry = mbr + MBR_TABLE_OFFSET;
	entry[4] = 0x0e;
	put32(entry + 8, 2048);
	put32(entry + 12, 262144);
	add_mbr_partition(mbr, 2, 0x82, 264192, 131072, 411648);
	entry = mbr + MBR_TABLE_OFFSET + MBR_ENTRY_SIZE;
	if (entry[4] != 0x82 || get32(entry + 8) != 264192 ||
	    get32(entry + 12) != 131072)
		fail("MBR self-test failed");
	entry = table;
	entry[0] = 0xa1;
	entry[1] = 0x91;
	pc98_chs(entry + 8, 2048, 8);
	pc98_chs(entry + 12, 411647, 8);
	if (!pc98_boot_entry_can_shorten(entry, 264192, 411648, 8, 264191) ||
	    pc98_boot_entry_can_shorten(entry, 264192, 411648, 8, 264190) ||
	    pc98_boot_entry_can_shorten(entry, 264192, 411649, 8, 264191))
		fail("PC-98 boot-boundary predicate self-test failed");
	entry[0] = 0xa0;
	if (pc98_boot_entry_can_shorten(entry, 264192, 411648, 8, 264191))
		fail("PC-98 non-FAT boundary self-test failed");
	entry[0] = 0xa1;
	add_pc98_partition(table, 2, 1, 264192, 131072, 411648, 8,
	    264191);
	entry = table;
	if (pc98_lba(entry + 12, 8) != 264191)
		fail("PC-98 boot-boundary self-test failed");
	entry = table + PC98_ENTRY_SIZE;
	if (pc98_lba(entry + 8, 8) != 264192 ||
	    pc98_lba(entry + 12, 8) != 395263 ||
	    memcmp(entry + 16, "SWAP", 4) != 0)
		fail("PC-98 self-test failed");
	if (parse_uuid("A1B2-C3D4") != 0xa1b2c3d4U)
		fail("FAT UUID self-test failed");
	if (gpt_crc32((const uint8_t *)"123456789", 9) != 0xcbf43926U)
		fail("GPT CRC32 self-test failed");
	make_gpt_partition_entry(gpt_entry, sizeof(gpt_entry), 3, 0,
	    264192, 32768);
	if (all_zero(gpt_entry, 16) || all_zero(gpt_entry + 16, 16) ||
	    get64(gpt_entry + 32) != 264192 ||
	    get64(gpt_entry + 40) != 296959 ||
	    memcmp(gpt_entry + 56, "B\0R\0-\0T\0", 8) != 0)
		fail("GPT partition-entry self-test failed");
	memcpy(gpt.header, "EFI PART", 8);
	put32(gpt.header + 8, 0x00010000U);
	put32(gpt.header + 12, GPT_HEADER_SIZE_MIN);
	update_gpt_header_crc(&gpt, 0x12345678U);
	header_crc = get32(gpt.header + 16);
	put32(gpt.header + 16, 0);
	if (header_crc == 0 || get32(gpt.header + 88) != 0x12345678U ||
	    gpt_crc32(gpt.header, GPT_HEADER_SIZE_MIN) != header_crc)
		fail("GPT header-CRC self-test failed");
	make_swap_v2_header(swap_header, 16U * 1024U * 1024U);
	if (memcmp(swap_header, "ZEDSWAP2", 8) != 0 ||
	    get16(swap_header + 8) != 2 ||
	    get16(swap_header + 10) != SWAP_HEADER_SIZE ||
	    get32(swap_header + 12) != SWAP_PAGE_SIZE ||
	    get64(swap_header + 16) != 16U * 1024U * 1024U ||
	    get64(swap_header + 24) != 4095U ||
	    get32(swap_header + SWAP_CHECKSUM_OFFSET) !=
	    swap_checksum(swap_header))
		fail("ZEDSWAP2 self-test failed");
	puts("boot-parameter-image-tool: PASS");
}

static void
decode_vram(const char *path)
{
	uint8_t screen[8192];
	char flat[80U * 25U + 1U];
	int descriptor;
	unsigned row, column, flat_at = 0;

	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		fail_errno(path);
	if (file_size(descriptor, path) < sizeof(screen))
		fail("PC-98 text VRAM snapshot is shorter than 8192 bytes");
	read_exact(descriptor, 0, screen, sizeof(screen), path);
	if (close(descriptor) != 0)
		fail_errno(path);
	for (row = 0; row < 25U; row++) {
		char line[80];
		unsigned end = 80U;

		for (column = 0; column < 80U; column++) {
			uint8_t code = screen[(row * 80U + column) * 2U];
			line[column] = code >= 0x20U && code <= 0x7eU ?
			    (char)code : ' ';
		}
		while (end != 0 && line[end - 1U] == ' ')
			end--;
		if (end != 0) {
			if (fwrite(line, 1, end, stdout) != end || putchar('\n') == EOF)
				fail_errno("stdout");
		}
		/* Preserve the complete 80-column stride in the flat form.  A
		 * kernel marker may wrap at column 80, so trimming individual rows
		 * would concatenate words or remove the separator at the wrap. */
		for (column = 0; column < 80U; column++)
			flat[flat_at++] = line[column];
	}
	while (flat_at != 0 && flat[flat_at - 1U] == ' ')
		flat_at--;
	flat[flat_at] = '\0';
	if (flat_at != 0)
		printf("BR-T46-VRAM-FLAT %s\n", flat);
}

int
main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "self-test")) {
		self_test();
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "add-partition")) {
		add_partition(argc, argv);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "set-fat-uuid")) {
		set_fat_uuid(argc, argv);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "stamp-swap-v2")) {
		stamp_swap_v2(argc, argv);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "decode-pc98-vram")) {
		decode_vram(argv[2]);
		return 0;
	}
	fprintf(stderr,
	    "usage:\n"
	    "  %s self-test\n"
	    "  %s add-partition --machine pcat|pc98 --kind swap|ufs "
	    "--index N --start-lba N --payload FILE IMAGE\n"
	    "  %s set-fat-uuid --partition-lba N --uuid XXXX-XXXX IMAGE\n"
	    "  %s stamp-swap-v2 --partition-index N IMAGE\n"
	    "  %s decode-pc98-vram SNAPSHOT\n",
	    argv[0], argv[0], argv[0], argv[0], argv[0]);
	return 2;
}
