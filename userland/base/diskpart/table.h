/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DISKPART_TABLE_H
#define ZEDBSD_DISKPART_TABLE_H
#include <stddef.h>
#include <stdint.h>

#define DP_MBR 1
#define DP_GPT 2
#define DP_DEGRADED 1U
#define DP_UNSUPPORTED 2U
#define DP_MAX_SLOTS 4096U

struct dp_io {
	void *context;
	uint64_t sectors;
	uint32_t sector_size;
	int (*read)(void *, uint64_t, void *, size_t);
	int (*write)(void *, uint64_t, const void *, size_t);
	int (*flush)(void *);
};
struct dp_part {
	uint64_t start, count, attributes;
	unsigned slot;
	char type[37], uuid[37], name[109];
};
struct dp_copy {
	uint8_t *header, *entries;
	uint64_t lba, alternate, first, last, table_lba;
	uint32_t slots, entry_size, header_size;
	size_t bytes;
};
struct dp_table {
	struct dp_io io;
	unsigned format, restrictions, slots, count;
	uint8_t *mbr, *edited;
	struct dp_copy copy[2];
	struct dp_part *parts;
	unsigned selected;
	int changed;
};
int dp_load(struct dp_table *, const struct dp_io *);
void dp_free(struct dp_table *);
int dp_guid_parse(const char *, uint8_t[16]);
void dp_guid_text(const uint8_t[16], char[37]);
uint32_t dp_crc32(const void *, size_t);
int dp_add(struct dp_table *, unsigned, uint64_t, uint64_t,
	const char *, const char *, const char *);
int dp_delete(struct dp_table *, unsigned);
/* Persist metadata only; does not issue reload or promise crash atomicity.
 * started distinguishes preflight rejection from potentially partial writes. */
int dp_write(struct dp_table *, int *started);
#endif
