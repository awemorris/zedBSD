/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

enum ufs_initial_block_kind {
	UFS_INITIAL_XATTR,
	UFS_INITIAL_DIRECTORY
};

struct ufs_initial_allocation {
	struct ufs_inode_metadata_images images;
	struct ufs_transaction_outcome outcome;
	struct quota_charge charge;
	uint64_t fragment;
	uint32_t cg;
};

static int initial_block_candidate(struct inode *inode, struct ufs_initial_allocation *group);
static int initial_block_locked(struct inode *inode, enum ufs_initial_block_kind kind, const uint8_t *area, size_t length, struct ufs_initial_allocation *group);
static int initial_block_group(struct inode *inode, enum ufs_initial_block_kind kind, const uint8_t *area, size_t length, int *handled);
static int directory_backing_group(struct inode *inode, int *handled);
static int xattr_allocate_group(struct inode *inode, const uint8_t *area, size_t length, int *handled);

/* Selects one free block and edits only its private allocation map. */
static int
initial_block_candidate(
	struct inode *inode,
	struct ufs_initial_allocation *group)
{
	struct ufs_mount_state *ms;
	uint32_t attempt;
	uint32_t cg;
	uint32_t local;
	uint32_t ndblk;
	uint32_t free_blocks;
	uint8_t *map;
	unsigned n;
	int error;

	/* Scans valid allocation groups while the caller excludes other allocators. */
	ms = state(inode->i_mount);
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;
		error = load_cg_locked(inode->i_mount, cg);
		if (error != 0)
			return error;
		ndblk = cg_ndblk(&ms->super, cg);
		map = ms->cg + ms->cg_freeoff;
		local = (ms->super.dblkno + ms->super.frag - 1U) & ~(ms->super.frag - 1U);

		/* Requires a complete free filesystem block, without consuming fragments. */
		while (local < ndblk && ms->super.frag <= ndblk - local) {
			for (n = 0; n < ms->super.frag; n++) {
				if (!bit_test(map, local + n))
					break;
			}
			if (n == ms->super.frag) {
				free_blocks = ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);
				if (free_blocks == 0 || ms->super.cstotal_nbfree == 0)
					return EIO;
				memcpy(group->images.cg[0], ms->cg, ms->super.bsize);

				/* Reserves only private bitmap bytes until the entire group commits. */
				for (n = 0; n < ms->super.frag; n++)
					bit_clear(group->images.cg[0] + ms->cg_freeoff, local + n);
				ufs_put32(group->images.cg[0], UFS_CG_NBFREE, free_blocks - 1U, ms->super.swapped);
				group->fragment = cgstart(&ms->super, cg) + local;
				group->cg = cg;
				return 0;
			}
			local += ms->super.frag;
		}
	}

	/* Reports exhausted full-block capacity without changing live accounting. */
	return ENOSPC;
}

/* Publishes allocation, initialized metadata and its inode reference together. */
static int
initial_block_locked(
	struct inode *inode,
	enum ufs_initial_block_kind kind,
	const uint8_t *area,
	size_t length,
	struct ufs_initial_allocation *group)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	struct ufs_inode_metadata_images *images;
	struct ufs_journal_extent extents[4];
	uint64_t dinode_fragment;
	unsigned n;
	int error;

	/* Checks the requested owner before selecting backing for its first block. */
	ms = state(inode->i_mount);
	ui = info(inode);
	images = &group->images;
	if (!ms->writable)
		return EROFS;
	if (ui->blocks > UINT64_MAX - ms->super.bsize / UFS_SECTOR_SIZE)
		return EIO;
	if (kind == UFS_INITIAL_XATTR) {
		if (ui->extattr_size != 0 || ui->extattr[0] != 0 || ui->extattr[1] != 0)
			return EIO;
	} else {
		if (inode->i_type != INODE_DIR || inode->i_size != 0)
			return EIO;
		for (n = 0; n < UFS_NDADDR; n++) {
			if (ui->direct[n] != 0)
				return EIO;
		}
		for (n = 0; n < UFS_NIADDR; n++) {
			if (ui->indirect[n] != 0)
				return EIO;
		}
	}
	error = initial_block_candidate(inode, group);
	if (error != 0)
		return error;

	/* Prepares a fully initialized payload and a private reference to its reservation. */
	memset(images->data, 0, ms->super.bsize);
	if (length != 0)
		memcpy(images->data, area, length);
	memcpy(&images->image, ui, sizeof(images->image));
	if (kind == UFS_INITIAL_XATTR) {
		images->image.extattr[0] = group->fragment;
		images->image.extattr_size = (uint32_t)length;
	} else {
		images->image.direct[0] = group->fragment;
	}
	images->image.blocks += ms->super.bsize / UFS_SECTOR_SIZE;
	error = prepare_inode_locked(&images->image.inode, images->dinode, &dinode_fragment);
	if (error != 0)
		return error;
	error = prepare_super_summaries(inode->i_mount, images->summaries);
	if (error != 0)
		return error;
	ufs_put64(images->summaries, UFS_FS_CSTOTAL_NBFREE,
	    ms->super.cstotal_nbfree - 1U, ms->super.swapped);

	/* Orders every home mutation behind one validated commit record. */
	extents[0].target = (cgstart(&ms->super, group->cg) + ms->super.cblkno) << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = images->cg[0];
	extents[1].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	extents[1].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	extents[1].payload = images->summaries;
	extents[2].target = group->fragment << ms->super.fsbtodb;
	extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[2].payload = images->data;
	extents[3].target = dinode_fragment << ms->super.fsbtodb;
	extents[3].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[3].payload = images->dinode;
	ms->cg_valid = 0;
	buf_view_release(&ms->cg_view);
	error = metadata_group_commit(inode->i_mount, extents, 4, NULL, &group->outcome);

	/* Makes the initialized area visible in RAM only after positive commit. */
	if (group->outcome.committed) {
		memcpy(ms->cg, images->cg[0], ms->super.bsize);
		ms->super.cstotal_nbfree--;
		ms->rotor_cg = group->cg;
		if (kind == UFS_INITIAL_XATTR) {
			ui->extattr[0] = group->fragment;
			ui->extattr_size = (uint32_t)length;
		} else {
			ui->direct[0] = group->fragment;
		}
		ui->blocks = images->image.blocks;
	}
	if (group->outcome.committed || group->outcome.uncertain)
		ms->cg_dirty = group->outcome.uncertain;

	/* Preserves the original errno and its separately recorded ownership outcome. */
	return error;
}

/* Reserves memory and quota before admitting the first metadata block. */
static int
initial_block_group(
	struct inode *inode,
	enum ufs_initial_block_kind kind,
	const uint8_t *area,
	size_t length,
	int *handled)
{
	struct ufs_mount_state *ms;
	struct ufs_initial_allocation *group;
	size_t bytes;
	int error;

	/* Declines only unsupported profiles before any quota or disk mutation. */
	ms = state(inode->i_mount);
	*handled = 0;
	if (!ms->journal_enabled)
		return 0;
	*handled = 1;
	if (kind == UFS_INITIAL_XATTR) {
		if (area == NULL || length == 0 || length > ms->super.bsize)
			return EINVAL;
	} else if (kind != UFS_INITIAL_DIRECTORY || area != NULL || length != 0) {
		return EINVAL;
	}
	bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		*handled = 0;
		return 0;
	}

	/* Allocates all private images before reserving quota. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->images.memory = kern_malloc(bytes);
	if (group->images.memory == NULL) {
		kern_free(group);
		return ENOMEM;
	}
	group->images.cg[0] = group->images.memory;
	group->images.dinode = group->images.memory + ms->super.bsize;
	group->images.data = group->images.dinode + ms->super.bsize;
	group->images.summaries = group->images.data + ms->super.bsize;
	error = quota_reserve(&ms->quota, inode->i_uid, inode->i_gid, 1, 0, quota_now(), &group->charge);
	if (error == 0) {
		mutex_lock(&ms->lock);
		error = initial_block_locked(inode, kind, area, length, group);

		/* Retains quota for a possibly committed allocation until recovery settles it. */
		if (group->outcome.committed || group->outcome.uncertain)
			quota_commit(&group->charge);
		else
			quota_rollback(&group->charge);
		mutex_unlock(&ms->lock);
	}
	kern_free(group->images.memory);
	kern_free(group);

	/* Returns an admitted failure without retrying the compatibility allocator. */
	return error;
}

/* Allocates initialized attribute backing through the common first-block owner. */
static int
xattr_allocate_group(
	struct inode *inode,
	const uint8_t *area,
	size_t length,
	int *handled)
{
	int error;

	/* Preserves the xattr caller's admission and errno contract. */
	error = initial_block_group(inode, UFS_INITIAL_XATTR, area, length, handled);
	return error;
}

/* Allocates empty directory backing without publishing any directory entry. */
static int
directory_backing_group(
	struct inode *inode,
	int *handled)
{
	int error;

	/* Leaves size and link counts untouched until a later namespace operation. */
	error = initial_block_group(inode, UFS_INITIAL_DIRECTORY, NULL, 0, handled);
	return error;
}
