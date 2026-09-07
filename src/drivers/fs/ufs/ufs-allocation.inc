/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Bound operation-local ownership independently of filesystem geometry. */
#define UFS_ALLOCATION_BLOCKS 16U
#define UFS_ALLOCATION_BYTES 65536U

struct ufs_allocation_run {
	struct ufs_inode_info image;
	struct quota_charge charges[UFS_ALLOCATION_BLOCKS];
	uint8_t *memory;
	uint8_t *old_cg;
	uint8_t *old_leaf;
	uint8_t *new_leaf;
	uint8_t *dinode;
	uint8_t *summaries;
	uint8_t *tree_memory;
	uint8_t *tree_images[UFS_NIADDR];
	uint64_t tree_targets[UFS_NIADDR];
	unsigned tree_indices[UFS_NIADDR];
	unsigned tree_count;
	unsigned missing_nodes;
	unsigned missing_root;
	unsigned root_level;
	uint64_t leaf;
	uint64_t first;
	uint64_t old_total;
	unsigned index;
	unsigned count;
	unsigned reserved;
	unsigned published;
	unsigned grouped;
	unsigned committed;
	unsigned uncertain;
};

static int allocation_missing_path(struct ufs_allocation_run *run, const struct ufs_super *super, uint64_t logical, unsigned depth);
static int allocation_group_commit(struct inode *inode, struct ufs_allocation_run *run, const struct io_context *context);
static int allocation_run_leaf(struct inode *inode, uint64_t logical, unsigned *count, struct ufs_allocation_run *run);
static int allocation_run_reserve(struct inode *inode, struct ufs_allocation_run *run);
static int allocation_run_abort(struct inode *inode, struct ufs_allocation_run *run);
static void allocation_run_release(struct ufs_allocation_run *run, int retained);
static ssize_t allocation_write_run(struct inode *inode, const void *buffer, size_t length, uint64_t logical, const struct io_context *context);

/* Commits allocation ownership and references under one exact journal owner. */
static int
allocation_group_commit(
	struct inode *inode,
	struct ufs_allocation_run *run,
	const struct io_context *context)
{
	struct mount *mountp;
	struct ufs_mount_state *ms;
	struct ufs_journal_extent extents[3 + UFS_NIADDR];
	struct kern_test_fault_result fault;
	struct ufs_transaction_outcome outcome;
	uint64_t fragment;
	unsigned count;
	unsigned n;
	int error;

	/* Prepares only this operation's dinode while the shared mount lock is held. */
	mountp = inode->i_mount;
	ms = state(mountp);
	error = prepare_inode_locked(&run->image.inode, run->dinode, &fragment);
	if (error != 0)
		return error;
	error = prepare_super_summaries(mountp, run->summaries);
	if (error != 0)
		return error;
	if (KERN_TEST_FAULT(KERN_TEST_FAULT_UFS_CG_WRITE, UINT32_MAX, UINT32_MAX, &fault))
		return fault.error != 0 ? fault.error : EIO;

	/* Describes disjoint shared blocks; core validation precedes every redo write. */
	extents[0].target = (cgstart(&ms->super, ms->active_cg) + ms->super.cblkno) << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = ms->cg;
	extents[1].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	extents[1].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	extents[1].payload = run->summaries;
	extents[2].target = fragment << ms->super.fsbtodb;
	extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[2].payload = run->dinode;
	count = 3;
	if (run->tree_count != 0) {
		/* Includes every newly initialized node and its changed existing parent. */
		for (n = 0; n < run->tree_count; n++) {
			extents[count].target = run->tree_targets[n] << ms->super.fsbtodb;
			extents[count].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
			extents[count].payload = run->tree_images[n];
			count++;
		}
	} else if (run->leaf != 0) {
		extents[3].target = run->leaf << ms->super.fsbtodb;
		extents[3].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extents[3].payload = run->new_leaf;
		count++;
	}

	error = metadata_group_commit(mountp, extents, count, context, &outcome);
	run->committed = outcome.committed;
	run->uncertain = outcome.uncertain;
	return error;
}

/* Prepares an absent suffix without installing empty nodes or live references. */
static int
allocation_missing_path(
	struct ufs_allocation_run *run,
	const struct ufs_super *super,
	uint64_t logical,
	unsigned depth)
{
	uint64_t divisor;
	unsigned n;
	unsigned slot;

	/* Allocates all private path images before quota or bitmap mutation. */
	run->tree_memory = kern_malloc(UFS_NIADDR * super->bsize);
	if (run->tree_memory == NULL)
		return ENOMEM;
	memset(run->tree_memory, 0, UFS_NIADDR * super->bsize);
	for (n = 0; n < UFS_NIADDR; n++)
		run->tree_images[n] = run->tree_memory + n * super->bsize;
	if (run->tree_count != 0)
		memcpy(run->tree_images[0], run->old_leaf, super->bsize);
	run->missing_nodes = depth;

	/* Records the pointer slot in each missing level, ending at a private leaf. */
	while (depth != 0) {
		divisor = 1;
		for (n = 1; n < depth; n++)
			divisor *= super->nindir;
		slot = run->tree_count++;
		run->tree_indices[slot] = (unsigned)(logical / divisor);
		logical %= divisor;
		depth--;
	}
	run->index = run->tree_indices[run->tree_count - 1U];
	run->new_leaf = run->tree_images[run->tree_count - 1U];

	/* Leaves every physical address and inode root unpublished until reservation. */
	return 0;
}

/* Locates a leaf or prepares its missing path without publishing empty nodes. */
static int
allocation_run_leaf(
	struct inode *inode,
	uint64_t logical,
	unsigned *count,
	struct ufs_allocation_run *run)
{
	const struct ufs_super *super;
	struct ufs_inode_info *ui;
	uint64_t span;
	uint64_t divisor;
	uint64_t fragment;
	unsigned level;
	unsigned depth;
	unsigned index;
	unsigned n;
	uint64_t parent = 0;
	unsigned parent_index = 0;
	int error;

	/* Limit direct allocations to the current array of zero pointers. */
	super = &state(inode->i_mount)->super;
	ui = info(inode);
	if (logical < UFS_NDADDR) {
		run->index = (unsigned)logical;
		for (n = 0; n < *count; n++) {
			if (logical + n >= UFS_NDADDR || ui->direct[logical + n] != 0)
				break;
		}
		*count = n;
		return 0;
	}

	/* Select the indirect root and its checked logical range. */
	logical -= UFS_NDADDR;
	span = super->nindir;
	for (level = 0; level < UFS_NIADDR; level++) {
		if (logical < span)
			break;
		logical -= span;
		if (span > UINT64_MAX / super->nindir)
			return EOVERFLOW;
		span *= super->nindir;
	}

	/* Traverses existing nodes, preparing an absent suffix only for grouped owners. */
	if (level == UFS_NIADDR)
		return EFBIG;
	fragment = ui->indirect[level];
	for (depth = level + 1U; depth != 0; depth--) {
		if (fragment == 0) {
			if (!run->grouped) {
				*count = 0;
				return 0;
			}
			run->root_level = level;
			run->missing_root = parent == 0;
			if (parent != 0) {
				run->tree_count = 1;
				run->tree_targets[0] = parent;
				run->tree_indices[0] = parent_index;
			}
			error = allocation_missing_path(run, super, logical, depth);
			if (error != 0)
				return error;
			if (*count > super->nindir - run->index)
				*count = super->nindir - run->index;
			return 0;
		}
		error = read_block(inode->i_mount, fragment, run->old_leaf);
		if (error != 0)
			return error;

		/* Consume one level while retaining the last leaf's original bytes. */
		divisor = 1;
		for (n = 1; n < depth; n++)
			divisor *= super->nindir;
		index = (unsigned)(logical / divisor);
		logical %= divisor;
		if (depth == 1) {
			run->leaf = fragment;
			run->index = index;
			break;
		}
		parent = fragment;
		parent_index = index;
		fragment = ufs_get64(run->old_leaf, index * 8U, super->swapped);
	}

	/* Stop at an occupied pointer or the end of this leaf. */
	for (n = 0; n < *count; n++) {
		if (run->index + n >= super->nindir)
			break;
		if (ufs_get64(run->old_leaf, (run->index + n) * 8U, super->swapped) != 0)
			break;
	}
	*count = n;
	memcpy(run->new_leaf, run->old_leaf, super->bsize);

	/* Report a private, unpublished leaf image. */
	return 0;
}

/* Reserve one contiguous prefix while retaining the mount allocator lock. */
static int
allocation_run_reserve(
	struct inode *inode,
	struct ufs_allocation_run *run)
{
	struct ufs_mount_state *ms;
	uint8_t *map;
	uint32_t cg;
	uint32_t attempt;
	uint32_t fragment;
	uint32_t ndblk;
	uint32_t free_blocks;
	unsigned count;
	unsigned n;
	int error;

	/* Search groups without exposing pending changes to another allocator. */
	ms = state(inode->i_mount);
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;
		error = load_cg_locked(inode->i_mount, cg);
		if (error != 0)
			return error;
		map = ms->cg + ms->cg_freeoff;
		ndblk = cg_ndblk(&ms->super, cg);

		/* Find the first complete free block and its bounded contiguous run. */
		for (fragment = ms->super.dblkno; fragment < ndblk; fragment += ms->super.frag) {
			count = 0;
			while (count < run->reserved) {
				if ((uint64_t)fragment + (count + 1U) * ms->super.frag > ndblk)
					break;
				for (n = 0; n < ms->super.frag; n++) {
					if (!bit_test(map, fragment + count * ms->super.frag + n))
						break;
				}
				if (n != ms->super.frag)
					break;
				count++;
			}
			if (count <= run->missing_nodes)
				continue;

			/* Save the entire current group before changing its ownership. */
			free_blocks = ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);
			if (free_blocks < count || ms->super.cstotal_nbfree < count)
				return EIO;
			memcpy(run->old_cg, ms->cg, ms->super.bsize);
			run->old_total = ms->super.cstotal_nbfree;
			run->count = count;
			run->first = cgstart(&ms->super, cg) + fragment;

			/* Remove the run from the single group and its global summary. */
			for (n = 0; n < count * ms->super.frag; n++)
				bit_clear(map, fragment + n);
			ufs_put32(ms->cg, UFS_CG_NBFREE, free_blocks - count, ms->super.swapped);
			ms->super.cstotal_nbfree -= count;
			return 0;
		}
	}

	/* Report exhaustion without changing allocation ownership. */
	return ENOSPC;
}

/* Confirm pointer removal before returning any uncertain allocation to the CG. */
static int
allocation_run_abort(
	struct inode *inode,
	struct ufs_allocation_run *run)
{
	struct ufs_mount_state *ms;
	int error;

	/* Restore original reachability when a metadata write may have landed. */
	ms = state(inode->i_mount);
	error = 0;
	if (run->published) {
		if (run->leaf != 0)
			error = write_block(inode->i_mount, run->leaf, run->old_leaf);
		if (error == 0)
			error = persist_inode_locked(inode);
		if (error == 0)
			error = disk_sync(inode->i_mount->m_disk);
	}

	/* Keep allocations charged and stop writes if reachability is uncertain. */
	if (error != 0) {
		ms->writable = 0;
		return error;
	}

	/* Restore allocation summaries only after no durable pointer can refer here. */
	memcpy(ms->cg, run->old_cg, ms->super.bsize);
	ms->super.cstotal_nbfree = run->old_total;
	error = write_cg(inode->i_mount);
	if (error == 0)
		error = disk_sync(inode->i_mount->m_disk);
	if (error != 0)
		ms->writable = 0;

	/* Let the caller retain charges when rollback durability remains uncertain. */
	return error;
}

/* End every quota reservation and release private working memory. */
static void
allocation_run_release(
	struct ufs_allocation_run *run,
	int retained)
{
	unsigned n;

	/* Commit only actual retained blocks and roll back unused prefix charges. */
	for (n = 0; n < run->reserved; n++) {
		if (retained && n < run->count)
			quota_commit(&run->charges[n]);
		else
			quota_rollback(&run->charges[n]);
	}
	kern_free(run->tree_memory);
	kern_free(run->memory);
	kern_free(run);
}

/* Initialize full new blocks before publishing a bounded private pointer image. */
static ssize_t
allocation_write_run(
	struct inode *inode,
	const void *buffer,
	size_t length,
	uint64_t logical,
	const struct io_context *context)
{
	struct ufs_mount_state *ms;
	struct ufs_allocation_run *run;
	struct ufs_inode_info *ui;
	size_t bytes;
	unsigned count;
	unsigned n;
	unsigned next;
	int error;
	int rollback;
	int retained;

	/* Bound content by the current transfer and journal payload contracts. */
	ms = state(inode->i_mount);
	bytes = length < UFS_ALLOCATION_BYTES ? length : UFS_ALLOCATION_BYTES;
	if (ms->journal_enabled && bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)
		bytes = (ms->journal.sector_count - 2U) * UFS_SECTOR_SIZE;
	count = (unsigned)(bytes / ms->super.bsize);
	if (count > UFS_ALLOCATION_BLOCKS)
		count = UFS_ALLOCATION_BLOCKS;
	if (count == 0)
		return 0;

	/* Declines an oversized group before changing either quota or allocation state. */
	if (ms->journal_enabled) {
		bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;
		if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
		    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)
			return 0;
	}

	/* Decline optimization before reservation when bounded memory is unavailable. */
	run = kern_calloc(1, sizeof(*run));
	if (run == NULL)
		return 0;
	run->grouped = ms->journal_enabled;
	run->memory = kern_malloc((run->grouped ? 4U : 3U) * ms->super.bsize +
	    (run->grouped ? UFS_SBLOCK_SIZE : 0));
	if (run->memory == NULL) {
		kern_free(run);
		return 0;
	}
	run->old_cg = run->memory;
	run->old_leaf = run->memory + ms->super.bsize;
	run->new_leaf = run->old_leaf + ms->super.bsize;
	if (run->grouped) {
		run->dinode = run->new_leaf + ms->super.bsize;
		run->summaries = run->dinode + ms->super.bsize;
	}
	/* Owns shared path images from their first read through publication or rollback. */
	mutex_lock(&ms->lock);
	if (!ms->writable) {
		mutex_unlock(&ms->lock);
		allocation_run_release(run, 0);
		return -EROFS;
	}
	error = allocation_run_leaf(inode, logical, &count, run);
	if (error != 0 || count == 0) {
		mutex_unlock(&ms->lock);
		allocation_run_release(run, 0);
		return -error;
	}

	/* Preflights every changed path block before reserving quota or allocation. */
	if (run->tree_count != 0) {
		bytes = (2U + run->tree_count) * ms->super.bsize + UFS_SBLOCK_SIZE;
		if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
		    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
			mutex_unlock(&ms->lock);
			allocation_run_release(run, 0);
			return 0;
		}
		if (count > UFS_ALLOCATION_BLOCKS - run->missing_nodes)
			count = UFS_ALLOCATION_BLOCKS - run->missing_nodes;
		count += run->missing_nodes;
	}

	/* Reserve quotas individually so a hard limit still permits a valid prefix. */
	for (n = 0; n < count; n++) {
		error = quota_reserve(&ms->quota, inode->i_uid, inode->i_gid,
		    1, 0, quota_now(), &run->charges[n]);
		if (error != 0)
			break;
		run->reserved++;
	}
	if (run->reserved <= run->missing_nodes) {
		mutex_unlock(&ms->lock);
		allocation_run_release(run, 0);
		return -error;
	}

	/* Reserves bitmap ownership under the same lock as the prepared reference path. */
	error = allocation_run_reserve(inode, run);
	if (error != 0) {
		mutex_unlock(&ms->lock);
		if (error == ENOSPC && run->missing_nodes != 0)
			error = 0;
		allocation_run_release(run, 0);
		return -error;
	}
	io_stats_record(IO_UFS_ALLOC_BEGIN, 0);
	bytes = (run->count - run->missing_nodes) * ms->super.bsize;
	for (n = 0; n < run->count; n++)
		io_stats_record(IO_UFS_ALLOCATE, ms->super.bsize);

	/* Keeps grouped allocation private until initialized data is durable. */
	if (run->grouped) {
		ms->cg_valid = 0;
		ms->cg_dirty = 1;
		buf_view_release(&ms->cg_view);
		error = 0;
	} else {
		error = write_cg(inode->i_mount);
	}
	if (error == 0) {
		io_stats_record(IO_UFS_CONTENT_WRITE, bytes);
		error = write_sectors_context(inode->i_mount, run->first << ms->super.fsbtodb,
		    (uint32_t)(bytes / UFS_SECTOR_SIZE), buffer, context);
	}
	if (error == 0)
		error = disk_sync(inode->i_mount->m_disk);

	/* Prepare the new inode and leaf without changing the published inode. */
	ui = info(inode);
	memcpy(&run->image, ui, sizeof(run->image));
	if (run->tree_count != 0) {
		next = run->count - run->missing_nodes;

		/* Assigns reserved metadata addresses only to the private path images. */
		for (n = 0; n < run->tree_count; n++) {
			if (run->tree_targets[n] == 0)
				run->tree_targets[n] = run->first + next++ * ms->super.frag;
		}
		for (n = 0; n + 1U < run->tree_count; n++) {
			ufs_put64(run->tree_images[n], run->tree_indices[n] * 8U,
			    run->tree_targets[n + 1U], ms->super.swapped);
		}
		if (run->missing_root)
			run->image.indirect[run->root_level] = run->tree_targets[0];
		run->leaf = run->tree_targets[run->tree_count - 1U];
	}
	for (n = 0; n < run->count - run->missing_nodes; n++) {
		if (run->leaf != 0) {
			ufs_put64(run->new_leaf, (run->index + n) * 8U,
			    run->first + n * ms->super.frag, ms->super.swapped);
		} else {
			run->image.direct[run->index + n] = run->first + n * ms->super.frag;
		}
	}
	run->image.blocks += (uint64_t)run->count * ms->super.bsize / UFS_SECTOR_SIZE;
	if ((uint64_t)run->image.inode.i_size < logical * ms->super.bsize + bytes)
		run->image.inode.i_size = (off_t)(logical * ms->super.bsize + bytes);

	/* Publish initialized pointers, retaining the old image until the flush passes. */
	if (error == 0 && run->grouped) {
		error = allocation_group_commit(inode, run, context);
	} else if (error == 0) {
		run->published = 1;
		if (run->leaf != 0)
			error = write_block(inode->i_mount, run->leaf, run->new_leaf);
		if (error == 0)
			error = persist_inode_locked(&run->image.inode);
		if (error == 0)
			error = disk_sync(inode->i_mount->m_disk);
	}

	/* Publish in-memory state only after a durable private metadata image. */
	retained = 1;
	if (error == 0 || run->committed) {
		memcpy(ui->direct, run->image.direct, sizeof(ui->direct));
		memcpy(ui->indirect, run->image.indirect, sizeof(ui->indirect));
		ui->blocks = run->image.blocks;
		inode->i_size = run->image.inode.i_size;
		ms->rotor_cg = ms->active_cg;
		if (run->grouped)
			ms->cg_dirty = run->uncertain;
		io_stats_record(IO_UFS_ALLOC_COMMIT, 0);
	} else if (run->grouped) {
		/* Uncommitted private ownership needs no compensating disk transaction. */
		if (!run->uncertain) {
			memcpy(ms->cg, run->old_cg, ms->super.bsize);
			ms->super.cstotal_nbfree = run->old_total;
			ms->cg_dirty = 0;
			retained = 0;
		}
		io_stats_record(IO_UFS_ALLOC_ABORT, 0);
	} else {
		rollback = allocation_run_abort(inode, run);
		retained = rollback != 0;
		io_stats_record(IO_UFS_ALLOC_ABORT, 0);
	}
	mutex_unlock(&ms->lock);
	allocation_run_release(run, retained);

	/* Return only completely initialized and published content. */
	if (error != 0)
		return -error;
	return (ssize_t)bytes;
}
