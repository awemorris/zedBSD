/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_KERN_UFS_CONSISTENCY_H
#define ZEDBSD_KERN_UFS_CONSISTENCY_H

#include <stddef.h>
#include <stdint.h>

enum ufs_consistency_mode {
	UFS_CONSISTENCY_SYNC = 0,
	UFS_CONSISTENCY_JOURNAL,
	UFS_CONSISTENCY_SOFTDEP,
	UFS_CONSISTENCY_SUJ,
};

struct ufs_journal_io {
	void *context;
	int (*read)(void *, uint64_t, uint32_t, void *);
	int (*write)(void *, uint64_t, uint32_t, const void *);
	int (*flush)(void *);
};

struct ufs_journal {
	struct ufs_journal_io io;
	uint64_t first_sector;
	uint32_t sector_count;
	uint64_t next_sequence;
	int poisoned;
};

int
ufs_journal_init(
	struct ufs_journal *journal,
	const struct ufs_journal_io *io,
	uint64_t first,
	uint32_t count);

/*
 * A journal has one durable transaction slot.  Its owner must serialize
 * commit and replay calls; the core deliberately has no scheduler/lock
 * dependency so host recovery tools can share it.
 */
int
ufs_journal_commit(
	struct ufs_journal *journal,
	uint64_t target,
	const void *payload,
	uint32_t sectors);

int
ufs_journal_replay(
	struct ufs_journal *journal);

#define UFS_SOFTDEP_MAX	64U
typedef int (
	*ufs_softdep_write_fn)(
	void *,
	uint64_t);

struct ufs_softdep_entry {
	uint64_t id;
	uint64_t block;
	uint64_t prerequisites;
	unsigned active;
};

struct ufs_softdep {
	struct ufs_softdep_entry entries[UFS_SOFTDEP_MAX];
	unsigned count;
	uint64_t next_id;
};

void
ufs_softdep_init(
	struct ufs_softdep *tracker);

int
ufs_softdep_add(
	struct ufs_softdep *tracker,
	uint64_t block,
	uint64_t *id);

int
ufs_softdep_depend(
	struct ufs_softdep *tracker,
	uint64_t after,
	uint64_t before);

int
ufs_softdep_drain(
	struct ufs_softdep *tracker,
	ufs_softdep_write_fn write,
	void *context);

#endif
