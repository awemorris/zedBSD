/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DRIVERS_FS_UFS2_CONSISTENCY_H
#define ZEDBSD_DRIVERS_FS_UFS2_CONSISTENCY_H

#include <stddef.h>
#include <stdint.h>

struct ufs2_journal_io {
	void *context;
	int (*read)(void *, uint64_t, uint32_t, void *);
	int (*write)(void *, uint64_t, uint32_t, const void *);
	int (*flush)(void *);
};

struct ufs2_journal {
	struct ufs2_journal_io io;
	uint64_t first_sector;
	uint32_t sector_count;
	uint64_t next_sequence;
	int poisoned;
};

int
ufs2_journal_init(
	struct ufs2_journal *journal,
	const struct ufs2_journal_io *io,
	uint64_t first,
	uint32_t count);

/*
 * A journal has one durable transaction slot.  Its owner must serialize
 * commit and replay calls; the core deliberately has no scheduler/lock
 * dependency so host recovery tools can share it.
 */
int
ufs2_journal_commit(
	struct ufs2_journal *journal,
	uint64_t target,
	const void *payload,
	uint32_t sectors);

int
ufs2_journal_replay(
	struct ufs2_journal *journal);

#define UFS2_SNAPSHOT_EMPTY UINT64_MAX

struct ufs2_snapshot_entry {
	uint64_t sector;
	uint32_t record;
	uint32_t reserved;
};

struct ufs2_snapshot {
	struct ufs2_journal_io io;
	uint64_t volume_sectors;
	uint64_t first_sector;
	uint32_t sector_count;
	uint32_t max_records;
	uint32_t next_record;
	struct ufs2_snapshot_entry *map;
	size_t map_count;
	unsigned active;
};

int
ufs2_snapshot_init(
	struct ufs2_snapshot *snapshot,
	const struct ufs2_journal_io *io,
	uint64_t volume,
	uint64_t first,
	uint32_t sectors,
	struct ufs2_snapshot_entry *map,
	size_t map_count);

int
ufs2_snapshot_open(
	struct ufs2_snapshot *snapshot);

int
ufs2_snapshot_create(
	struct ufs2_snapshot *snapshot);

int
ufs2_snapshot_preserve(
	struct ufs2_snapshot *snapshot,
	uint64_t first,
	uint32_t count);

int
ufs2_snapshot_read(
	struct ufs2_snapshot *snapshot,
	uint64_t first,
	uint32_t count,
	void *buffer);

int
ufs2_snapshot_delete(
	struct ufs2_snapshot *snapshot);

#endif
