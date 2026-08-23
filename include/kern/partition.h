/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Partition
 */

#ifndef ZEDBSD_KERN_PARTITION_H
#define ZEDBSD_KERN_PARTITION_H

#include "kern/disk.h"

#define PARTITION_MAX		16U
#define PARTITION_POOL_MAX	64U
#define PARTITION_LABEL_MAX	64U
#define PARTITION_UUID_MAX	64U
#define PARTITION_BOOTABLE	0x0001U
#define PARTITION_HAS_LABEL	0x0002U
#define PARTITION_HAS_UUID	0x0004U

struct partition {
	struct disk *p_parent;
	struct disk *p_disk;
	unsigned p_index;
	uint64_t p_start_block;
	uint64_t p_data_block;
	uint64_t p_block_count;
	unsigned p_flags;
	char p_label[PARTITION_LABEL_MAX];
	char p_uuid[PARTITION_UUID_MAX];
};

struct partition_scheme {
	const char *name;
	int (*scan)(const struct partition_scheme *scheme, struct disk *disk, struct partition *entries, unsigned capacity);
};

void
partition_set_scheme(
	const struct partition_scheme *scheme);

const struct partition_scheme *
partition_get_scheme(void);

int
partition_scan(
	struct disk *disk,
	struct partition *entries,
	unsigned capacity);

int
partition_create_disk(
	struct partition *partition);

void
partition_reset(void);

unsigned
partition_count(void);

const struct partition *
partition_at(
	unsigned index);

#endif
