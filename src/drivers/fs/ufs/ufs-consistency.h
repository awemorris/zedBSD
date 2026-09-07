/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DRIVERS_FS_UFS_CONSISTENCY_H
#define ZEDBSD_DRIVERS_FS_UFS_CONSISTENCY_H

#include <stddef.h>
#include <stdint.h>

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
	uint64_t home_sectors;
	uint64_t pending_sequence;
	uint32_t pending_digest;
	unsigned pending_ready;
	unsigned pending_clearing;
	uint64_t committed_sequence;
	uint32_t committed_digest;
	uint8_t *image;
	unsigned image_valid;
	uint32_t image_readers;
};

/* A zero-initialized, noncopyable pin owns immutable bytes until view_release. */
struct ufs_journal_view {
	struct ufs_journal *journal;
	const uint8_t *image;
	uint64_t sequence;
	uint64_t home_sectors;
};

/* View operations alone may run concurrently with the serialized writer.
 * Retirement closes acquisition; backing cannot be reused until pins drain.
 * Copy refuses any uncovered sector before modifying the destination. */
int ufs_journal_view_acquire(struct ufs_journal *journal, struct ufs_journal_view *view);
int ufs_journal_view_copy(const struct ufs_journal_view *view, uint64_t first,
    uint32_t count, void *buffer);
void ufs_journal_view_release(struct ufs_journal_view *view);
int ufs_journal_views_busy(const struct ufs_journal *journal);
/* Writer/teardown owner closes acquisition, then drains pins before discarding backing. */
void ufs_journal_views_close(struct ufs_journal *journal);

int
ufs_journal_init(
	struct ufs_journal *journal,
	const struct ufs_journal_io *io,
	uint64_t first,
	uint32_t count,
	uint64_t home_sectors);

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

/*
 * Bounded multi-target redo is the sole journal format.
 * The owner serializes non-view calls and replays after initialization before mutation.
 * Payloads remain immutable through commitv. A nonempty slot returns EBUSY;
 * commit errors remain errors even when immediate recovery installs the group.
 */
#define UFS_JOURNAL_EXTENTS 30U
#define UFS_JOURNAL_GROUP_SECTORS 128U
#define UFS_JOURNAL_IMAGE_BYTES ((UFS_JOURNAL_GROUP_SECTORS + 1U) * 512U)

/* Owner accounts and retains this exclusive storage until an idle detach.
 * Non-view calls remain serialized; initialization must not discard a live owner. */
int ufs_journal_bind_image(struct ufs_journal *journal, void *image, size_t bytes);
struct ufs_journal_extent {
	uint64_t target;
	uint32_t sectors;
	const void *payload;
};
int ufs_journal_commitv(struct ufs_journal *journal,
    const struct ufs_journal_extent *extents, unsigned count);

/* Publication retains only durable redo and a witness, never caller payload pointers.
 * The serialized owner checkpoints or recovers pending state before destruction. */
int ufs_journal_publishv(struct ufs_journal *journal,
    const struct ufs_journal_extent *extents, unsigned count);
int ufs_journal_checkpoint(struct ufs_journal *journal);
/* Drains a prior owner, retries recovery, and retains the original error. */
int ufs_journal_drain(struct ufs_journal *journal);
int ufs_journal_read(struct ufs_journal *journal, uint64_t first,
    uint32_t count, void *buffer);

/* Positive commit proof survives slot retirement until another commit or init.
 * Inspect under owner serialization before admitting another group. A false
 * result alone is not proof that rollback is safe after unresolved I/O. */
int ufs_journal_committed(const struct ufs_journal *journal,
    uint64_t sequence, uint32_t digest);

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

int
ufs_snapshot_init(
	struct ufs_snapshot *snapshot,
	const struct ufs_journal_io *io,
	uint64_t volume,
	uint64_t first,
	uint32_t sectors,
	struct ufs_snapshot_entry *map,
	size_t map_count);

int
ufs_snapshot_open(
	struct ufs_snapshot *snapshot);

int
ufs_snapshot_create(
	struct ufs_snapshot *snapshot);

int
ufs_snapshot_preserve(
	struct ufs_snapshot *snapshot,
	uint64_t first,
	uint32_t count);

int
ufs_snapshot_read(
	struct ufs_snapshot *snapshot,
	uint64_t first,
	uint32_t count,
	void *buffer);

int
ufs_snapshot_delete(
	struct ufs_snapshot *snapshot);

#endif
