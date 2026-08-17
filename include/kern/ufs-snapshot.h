/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_UFS_SNAPSHOT_H
#define ZEDBSD_KERN_UFS_SNAPSHOT_H

#include "kern/ufs-consistency.h"

#include <stddef.h>
#include <stdint.h>

#define UFS_SNAPSHOT_EMPTY UINT64_MAX

struct ufs_snapshot_entry {
	uint64_t sector;
	uint32_t record;
	uint32_t reserved;
};

struct ufs_snapshot {
	struct ufs_journal_io io;
	uint64_t volume_sectors;
	uint64_t first_sector;
	uint32_t sector_count;
	uint32_t max_records;
	uint32_t next_record;
	struct ufs_snapshot_entry *map;
	size_t map_count;
	unsigned active;
};

int ufs_snapshot_init(struct ufs_snapshot *,const struct ufs_journal_io *,
	uint64_t,uint64_t,uint32_t,struct ufs_snapshot_entry *,size_t);
int ufs_snapshot_open(struct ufs_snapshot *);
int ufs_snapshot_create(struct ufs_snapshot *);
int ufs_snapshot_preserve(struct ufs_snapshot *,uint64_t,uint32_t);
int ufs_snapshot_read(struct ufs_snapshot *,uint64_t,uint32_t,void *);
int ufs_snapshot_delete(struct ufs_snapshot *);

#endif
