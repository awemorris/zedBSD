/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

struct ufs_release_group {
	struct ufs_inode_info image;
	uint8_t *memory;
	uint8_t *cg;
	uint8_t *dinode;
	uint8_t *parent;
	uint8_t *summaries;
};

static int release_group_locked(struct inode *inode, uint64_t parent, unsigned index, uint64_t child, struct ufs_release_group *group);
static int release_group(struct inode *inode, uint64_t parent, unsigned index, uint64_t child, int *handled);

/* Prepares a private pointer removal and free map under mount mutation ownership. */
static int
release_group_locked(
	struct inode *inode,
	uint64_t parent,
	unsigned index,
	uint64_t child,
	struct ufs_release_group *group)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_inode_info *ui = info(inode);
	struct ufs_journal_extent extents[4];
	struct ufs_transaction_outcome outcome;
	uint64_t start;
	uint64_t fragment;
	uint64_t current;
	uint32_t cg;
	uint32_t local = 0;
	uint32_t free_blocks;
	unsigned n;
	unsigned count;
	int error;
	int quota_error;

	/* Refuses malformed accounting before preparing any reusable allocation. */
	if (!ms->writable)
		return EROFS;
	if (ui->blocks < ms->super.bsize / UFS_SECTOR_SIZE)
		return EIO;
	for (cg = 0; cg < ms->super.ncg; cg++) {
		start = cgstart(&ms->super, cg);
		if (child < start || child - start < ms->super.dblkno ||
		    child - start >= cg_ndblk(&ms->super, cg))
			continue;
		local = (uint32_t)(child - start);
		if (local % ms->super.frag != 0 ||
		    ms->super.frag > cg_ndblk(&ms->super, cg) - local)
			return EIO;
		break;
	}
	if (cg == ms->super.ncg)
		return EIO;
	error = load_cg_locked(inode->i_mount, cg);
	if (error != 0)
		return error;
	memcpy(group->cg, ms->cg, ms->super.bsize);
	for (n = 0; n < ms->super.frag; n++) {
		if (bit_test(group->cg + ms->cg_freeoff, local + n))
			return EIO;
	}
	free_blocks = ufs_get32(group->cg, UFS_CG_NBFREE, ms->super.swapped);
	if (free_blocks == UINT32_MAX || ms->super.cstotal_nbfree == UINT64_MAX)
		return EIO;

	/* Validates the current reference and changes only a private inode or parent. */
	memcpy(&group->image, ui, sizeof(group->image));
	if (parent != 0) {
		if (index >= ms->super.nindir)
			return EIO;
		error = read_block(inode->i_mount, parent, group->parent);
		if (error != 0)
			return error;
		current = ufs_get64(group->parent, index * 8U, ms->super.swapped);
		ufs_put64(group->parent, index * 8U, 0, ms->super.swapped);
	} else if (index < UFS_NDADDR) {
		current = group->image.direct[index];
		group->image.direct[index] = 0;
	} else {
		if (index - UFS_NDADDR >= UFS_NIADDR)
			return EIO;
		current = group->image.indirect[index - UFS_NDADDR];
		group->image.indirect[index - UFS_NDADDR] = 0;
	}
	if (current != child)
		return EIO;
	group->image.blocks -= ms->super.bsize / UFS_SECTOR_SIZE;
	error = prepare_inode_locked(&group->image.inode, group->dinode, &fragment);
	if (error != 0)
		return error;
	error = prepare_super_summaries(inode->i_mount, group->summaries);
	if (error != 0)
		return error;

	/* Keeps the live free map unchanged until the release has committed. */
	for (n = 0; n < ms->super.frag; n++)
		bit_set(group->cg + ms->cg_freeoff, local + n);
	ufs_put32(group->cg, UFS_CG_NBFREE, free_blocks + 1U, ms->super.swapped);
	ufs_put64(group->summaries, UFS_FS_CSTOTAL_NBFREE,
	    ms->super.cstotal_nbfree + 1U, ms->super.swapped);
	extents[0].target = (cgstart(&ms->super, cg) + ms->super.cblkno) << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = group->cg;
	extents[1].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	extents[1].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	extents[1].payload = group->summaries;
	extents[2].target = fragment << ms->super.fsbtodb;
	extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[2].payload = group->dinode;
	count = 3;
	if (parent != 0) {
		extents[3].target = parent << ms->super.fsbtodb;
		extents[3].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extents[3].payload = group->parent;
		count++;
	}
	/* Releases optional home-cache pins before the journal installs that block. */
	ms->cg_valid = 0;
	buf_view_release(&ms->cg_view);
	error = metadata_group_commit(inode->i_mount, extents, count, NULL, &outcome);
	if (outcome.committed) {
		memcpy(ms->cg, group->cg, ms->super.bsize);
		ms->super.cstotal_nbfree++;
		memcpy(ui->direct, group->image.direct, sizeof(ui->direct));
		memcpy(ui->indirect, group->image.indirect, sizeof(ui->indirect));
		ui->blocks = group->image.blocks;
		quota_error = quota_release(&ms->quota, inode->i_uid, inode->i_gid, 1, 0);
		if (quota_error != 0) {
			ms->writable = 0;
			if (error == 0)
				error = quota_error;
		}
	}
	if (outcome.committed || outcome.uncertain) {
		ms->cg_valid = 0;
		ms->cg_dirty = outcome.uncertain;
		buf_view_release(&ms->cg_view);
	}

	/* Preserves errno separately from any release that recovery established. */
	return error;
}

/* Reserves bounded private storage before a journal-backed pointer release. */
static int
release_group(
	struct inode *inode,
	uint64_t parent,
	unsigned index,
	uint64_t child,
	int *handled)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_release_group *group;
	size_t bytes;
	int error;

	/* Keeps oversized or non-journal releases on the existing ordered path. */
	*handled = 0;
	bytes = (parent != 0 ? 3U : 2U) * ms->super.bsize + UFS_SBLOCK_SIZE;
	if (!ms->journal_enabled)
		return 0;
	if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)
		return 0;
	*handled = 1;
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(3U * ms->super.bsize + UFS_SBLOCK_SIZE);
	if (group->memory == NULL) {
		kern_free(group);
		return ENOMEM;
	}
	group->cg = group->memory;
	group->dinode = group->cg + ms->super.bsize;
	group->parent = group->dinode + ms->super.bsize;
	group->summaries = group->parent + ms->super.bsize;
	mutex_lock(&ms->lock);
	error = release_group_locked(inode, parent, index, child, group);
	mutex_unlock(&ms->lock);
	kern_free(group->memory);
	kern_free(group);

	/* Reports a complete bounded release or its original failure. */
	return error;
}
