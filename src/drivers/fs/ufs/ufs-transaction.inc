/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

struct ufs_transaction_outcome {
	unsigned committed;
	unsigned uncertain;
};

/* Owns one editable image per physical block during a metadata operation. */
struct ufs_metadata_images {
	struct mount *mountp;
	uint8_t *memory;
	size_t capacity;
	unsigned count;
	struct ufs_journal_extent extents[UFS_JOURNAL_EXTENTS];
};

static void metadata_images_init(struct ufs_metadata_images *images, struct mount *mountp, uint8_t *memory, size_t bytes);
static int metadata_image_get(struct ufs_metadata_images *images, uint64_t fragment, uint8_t **result);
static int metadata_image_inode(struct ufs_metadata_images *images, struct inode *prepared);
static int metadata_group_commit(struct mount *mountp, const struct ufs_journal_extent *extents, unsigned count, const struct io_context *context, struct ufs_transaction_outcome *outcome);

/* Serializes snapshot preservation, exact commit outcome and metadata durability. */
static int
metadata_group_commit(
	struct mount *mountp,
	const struct ufs_journal_extent *extents,
	unsigned count,
	const struct io_context *context,
	struct ufs_transaction_outcome *outcome)
{
	struct ufs_mount_state *ms = state(mountp);
	struct io_context child;
	uint64_t sequence;
	unsigned n;
	int error, deferred;

	memset(outcome, 0, sizeof(*outcome));
	/* Carries the logical owner through every snapshot and journal durability step. */
	error = io_context_child(&child, context, IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;
	/* Policy-off drains under ms->lock after closing this admission query. */
	deferred = context == NULL && writeback_mount_active(mountp);
	io_epoch_begin(&mountp->m_write_epoch);
	if (ms->snapshot_available) {
		mutex_lock(&ms->snapshot_lock);
		ms->snapshot_io.context = &child;

		/* Preserves all original homes before a grouped checkpoint may overwrite any. */
		for (n = 0; n < count; n++) {
			error = ufs_snapshot_preserve(&ms->snapshot, extents[n].target, extents[n].sectors);
			if (error != 0)
				break;
		}
		ms->snapshot_io.context = NULL;
	}
	if (error == 0) {
		mutex_lock(&ms->journal_lock);
		error = journal_checkpoint_locked(mountp);
		sequence = 0;
		if (error == 0) {
			journal_wait_readers(ms);
			ms->journal_io.context = &child;
			sequence = ms->journal.next_sequence;
			if (deferred) {
				error = ufs_journal_publishv(&ms->journal, extents, count);
				if (error != 0 && ms->journal.pending_sequence != 0)
					(void)ufs_journal_drain(&ms->journal);
			} else {
				error = ufs_journal_commitv(&ms->journal, extents, count);
			}
		}

		/* Sequence identity is unique until init; inspect before releasing admission. */
		outcome->committed = sequence != 0 && ms->journal.committed_sequence == sequence;
		outcome->uncertain = ms->journal.poisoned ||
		    (ms->journal.pending_sequence != 0 &&
		    !(error == 0 && outcome->committed && ms->journal.pending_ready));
		if (outcome->uncertain)
			ms->writable = 0;
		ms->journal_io.context = NULL;
		mutex_unlock(&ms->journal_lock);
	}
	if (ms->snapshot_available)
		mutex_unlock(&ms->snapshot_lock);
	io_epoch_end(&mountp->m_write_epoch);

	/* Returns the original errno independently of positive committed ownership. */
	return error;
}

/* Bounds a caller-owned image set by both memory and journal capacity. */
static void
metadata_images_init(
	struct ufs_metadata_images *images,
	struct mount *mountp,
	uint8_t *memory,
	size_t bytes)
{
	struct ufs_mount_state *ms;
	size_t sectors;

	/* Initializes an empty owner without allocating or publishing metadata. */
	memset(images, 0, sizeof(*images));
	ms = state(mountp);
	images->mountp = mountp;
	images->memory = memory;
	sectors = bytes / UFS_SECTOR_SIZE;

	/* Restricts admission to the active journal's payload and descriptor limits. */
	if (sectors > UFS_JOURNAL_GROUP_SECTORS)
		sectors = UFS_JOURNAL_GROUP_SECTORS;
	if (ms->journal.sector_count <= 2U)
		sectors = 0;
	else if (sectors > ms->journal.sector_count - 2U)
		sectors = ms->journal.sector_count - 2U;
	images->capacity = sectors * UFS_SECTOR_SIZE / ms->super.bsize;
	if (images->capacity > UFS_JOURNAL_EXTENTS)
		images->capacity = UFS_JOURNAL_EXTENTS;
}

/* Returns the unique private image of a block without losing earlier edits. */
static int
metadata_image_get(
	struct ufs_metadata_images *images,
	uint64_t fragment,
	uint8_t **result)
{
	struct ufs_mount_state *ms;
	struct ufs_journal_extent *extent;
	uint8_t *block;
	uint64_t sector;
	uint32_t sectors;
	unsigned n;
	int error;

	/* Validates the physical extent before matching it against existing owners. */
	*result = NULL;
	ms = state(images->mountp);
	sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	if (fragment == 0 || ms->super.fsbtodb >= 64U ||
	    fragment > (UINT64_MAX >> ms->super.fsbtodb))
		return EIO;
	sector = fragment << ms->super.fsbtodb;
	if (sector > UINT64_MAX - sectors)
		return EIO;

	/* Reuses identical blocks and rejects aliasing through a partial overlap. */
	for (n = 0; n < images->count; n++) {
		extent = &images->extents[n];
		if (extent->target == sector) {
			*result = images->memory + n * ms->super.bsize;
			return 0;
		}
		if (sector < extent->target + extent->sectors &&
		    extent->target < sector + sectors)
			return EIO;
	}

	/* Reads a new image only when its full block fits the reserved transaction. */
	if (images->count >= images->capacity)
		return ENOSPC;
	block = images->memory + images->count * ms->super.bsize;
	error = read_block(images->mountp, fragment, block);
	if (error != 0)
		return error;

	/* Admits only successfully loaded bytes and keeps the extent order stable. */
	extent = &images->extents[images->count];
	extent->target = sector;
	extent->sectors = sectors;
	extent->payload = block;
	images->count++;
	*result = block;

	/* Returns the image for in-place private preparation under the mount lock. */
	return 0;
}

/* Merges a prepared dinode into the unique image of its containing block. */
static int
metadata_image_inode(
	struct ufs_metadata_images *images,
	struct inode *prepared)
{
	uint8_t *block;
	int error;

	/* Rejects a foreign inode before interpreting its physical location. */
	if (prepared->i_mount != images->mountp)
		return EXDEV;

	/* Preserves any sibling edits already present in this private block. */
	error = metadata_image_get(images, inode_fragment(prepared), &block);
	if (error != 0)
		return error;
	encode_inode_locked(prepared, block);

	/* Leaves live inode publication to the operation's committed outcome. */
	return 0;
}
